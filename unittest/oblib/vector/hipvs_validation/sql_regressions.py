"""Behavior checks before/after a controlled database restart; touches only a new test database."""
import argparse
import json
import os
import re
import time
import uuid
from pathlib import Path

import numpy as np
from mysql_client import pymysql
from metrics import vector_literal, write_json


def ident(value):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*',value):raise ValueError('unsafe identifier in state file')
    return '`'+value+'`'


def query(cur,state,vector,k=10,tag=None):
    where='' if tag is None else ' WHERE tag='+str(int(tag))
    sql='SELECT id FROM '+ident(state['database'])+'.vectors'+where+' ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT '+str(k)
    cur.execute(sql,(vector_literal(vector),))
    return [int(row[0]) for row in cur.fetchall()]


def expected(state,vector,k=10,tag=None):
    rows=[r for r in state['rows'] if tag is None or r['tag']==tag]
    return [r['id'] for r in sorted(rows,key=lambda r:(float(np.sum((np.asarray(r['vector'],dtype=np.float64)-vector)**2)),r['id']))[:k]]


def wait_for(check,description,timeout=30):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if check():return
        time.sleep(0.2)
    raise AssertionError(description)


def verify(cur,state):
    cur.execute('SELECT COUNT(*) FROM '+ident(state['database'])+'.vectors')
    assert cur.fetchone()[0]==len(state['rows']),'row count after mutation/restart'
    for pos in (0,1,17,100,len(state['rows'])-1):
        row=state['rows'][pos]
        assert query(cur,state,row['vector'],1)==[row['id']],'self query/ID mapping'
    vector=state['rows'][17]['vector']
    for tag in (None,0,1):
        got=query(cur,state,vector,10,tag)
        assert len(got)==len(set(got))==10,'missing or duplicate filtered results'
        assert set(got)==set(expected(state,vector,10,tag)),'filtered/unfiltered exact small-fixture neighbors'


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--phase',choices=['prepare','verify'],required=True)
    p.add_argument('--state',required=True)
    p.add_argument('--port',type=int,default=2883)
    p.add_argument('--host',default='127.0.0.1')
    p.add_argument('--user',default='root')
    p.add_argument('--library',choices=['cuvs','vsag'],default='cuvs')
    a=p.parse_args()
    path=Path(a.state)
    with pymysql.connect(host=a.host,port=a.port,user=a.user,password=os.getenv('SEEKDB_PASSWORD',''),autocommit=True,read_timeout=120) as conn:
        with conn.cursor() as cur:
            if a.phase=='prepare':
                if path.exists():raise FileExistsError('state already exists; refusing to replace it')
                rng=np.random.Generator(np.random.PCG64(9123))
                vectors=rng.normal(size=(512,8)).astype(np.float32)
                state={'database':'amd_regression_'+uuid.uuid4().hex[:10],'rows':[
                    {'id':(-17 if i==0 else 0 if i==1 else 8589934593+17*i),'tag':i%2,'vector':[float(v) for v in vectors[i]]} for i in range(512)]}
                cur.execute('CREATE DATABASE '+ident(state['database']))
                cur.execute('USE '+ident(state['database']))
                cur.execute('CREATE TABLE vectors(id BIGINT PRIMARY KEY,tag INT,v VECTOR(8))')
                cur.executemany('INSERT INTO vectors VALUES(%s,%s,%s)',[(r['id'],r['tag'],vector_literal(r['vector'])) for r in state['rows']])
                cur.execute(f'CREATE VECTOR INDEX ann ON vectors(v) WITH(distance=l2,type=hnsw,lib={a.library},m=16,ef_construction=200,ef_search=1000)')
                verify(cur,state)
                target=state['rows'][7]
                target['vector']=[50.0]*8
                cur.execute('UPDATE vectors SET v=%s WHERE id=%s',(vector_literal(target['vector']),target['id']))
                wait_for(lambda:query(cur,state,target['vector'],1)==[target['id']],'updated vector not visible')
                removed=state['rows'].pop(9)
                cur.execute('DELETE FROM vectors WHERE id=%s',(removed['id'],))
                wait_for(lambda:removed['id'] not in query(cur,state,removed['vector']),'deleted ID still returned')
                added={'id':999999999999,'tag':1,'vector':[100.0]*8}
                cur.execute('INSERT INTO vectors VALUES(%s,%s,%s)',(added['id'],added['tag'],vector_literal(added['vector'])))
                state['rows'].append(added)
                wait_for(lambda:query(cur,state,added['vector'],1)==[added['id']],'new vector not visible')
                verify(cur,state)
                # The explicitly logged small-snapshot fallback must remain correct.
                cur.execute('CREATE TABLE small_vectors(id INT PRIMARY KEY,v VECTOR(8))')
                cur.executemany('INSERT INTO small_vectors VALUES(%s,%s)',[(i,vector_literal(vectors[i])) for i in range(20)])
                cur.execute(f'CREATE VECTOR INDEX small_ann ON small_vectors(v) WITH(distance=l2,type=hnsw,lib={a.library},m=16,ef_construction=200,ef_search=1000)')
                cur.execute('SELECT id FROM small_vectors ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 1',(vector_literal(vectors[4]),))
                assert cur.fetchone()==(4,),'small-snapshot CPU fallback query'
                # Empty snapshots must not loop forever in the same-prefix reload
                # guard, and their first delta insertion must remain queryable.
                cur.execute('CREATE TABLE empty_vectors(id BIGINT PRIMARY KEY,v VECTOR(8))')
                cur.execute(f'CREATE VECTOR INDEX empty_ann ON empty_vectors(v) WITH(distance=l2,type=hnsw,lib={a.library},m=16,ef_construction=200,ef_search=1000)')
                empty_sql='SELECT id FROM empty_vectors ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT 1'
                cur.execute(empty_sql,(vector_literal(vectors[0]),))
                assert cur.fetchall()==(),'empty indexed table must return promptly'
                cur.execute('INSERT INTO empty_vectors VALUES(%s,%s)',(77777777777,vector_literal(vectors[0])))
                cur.execute(empty_sql,(vector_literal(vectors[0]),))
                assert cur.fetchone()==(77777777777,),'first insert into empty indexed table'
                state['library']=a.library
                small_check='20 row CPU fallback' if a.library=='cuvs' else '20 row CPU index'
                state['checks_before_restart']=['512 row snapshot','64-bit,zero,negative primary keys','filter','update','delete','insert',small_check,'empty snapshot','first insert after empty snapshot']
                write_json(path,state)
                print(json.dumps({'database':state['database'],'phase':'prepare','passed':state['checks_before_restart']}),flush=True)
            else:
                state=json.loads(path.read_text())
                verify(cur,state)
                write_json(path.with_name(path.stem+'-restart-result.json'),{'database':state['database'],'phase':'after_restart','passed':True,
                    'checks':['persisted row count','self queries','negative/64-bit IDs','filtered Top-K','updated/inserted/deleted state']})
                print('Post-restart native-index regression passed',flush=True)


if __name__=='__main__':main()
