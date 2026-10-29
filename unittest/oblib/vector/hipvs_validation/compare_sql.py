"""Paired SQL end-to-end build measurements with per-round construction evidence.

Run on the database host, with no concurrent build/benchmark jobs. The configured
quality floor is a local validation criterion, never an invented contest target.
"""
import argparse
import hashlib
import json
import os
import re
import statistics
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

from mysql_client import pymysql
from benchmark import load_dataset, run_round
from metrics import score_results, write_json


def sha(path):
    digest=hashlib.sha256()
    with open(path,'rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--dataset',required=True)
    p.add_argument('--out',required=True)
    p.add_argument('--cpu-port',type=int,default=2881)
    p.add_argument('--gpu-port',type=int,default=2883)
    p.add_argument('--host',default='127.0.0.1')
    p.add_argument('--user',default='root')
    p.add_argument('--cpu-log',required=True)
    p.add_argument('--gpu-log',required=True)
    p.add_argument('--cpu-binary',required=True)
    p.add_argument('--gpu-binary',required=True)
    p.add_argument('--worker',required=True)
    p.add_argument('--rounds',type=int,default=3)
    p.add_argument('--m',type=int,default=32)
    p.add_argument('--ef-construction',type=int,default=200)
    p.add_argument('--ef-search',type=int,default=1000)
    p.add_argument('--min-recall',type=float,default=0.95)
    a=p.parse_args()
    if a.rounds<3 or not 0<a.min_recall<=1:p.error('need at least 3 repetitions and a valid local recall floor')
    manifest,base,queries,ids,truth=load_dataset(a.dataset)
    config=SimpleNamespace(m=a.m,ef_construction=a.ef_construction,ef_search=a.ef_search,k=manifest['k'])
    out=Path(a.out);out.mkdir(parents=True,exist_ok=False)
    summary={'utc':datetime.now(timezone.utc).isoformat(),'dataset':manifest,
             'parameters':vars(config),'rounds':[], 'local_min_recall':a.min_recall,
             'official_target_speedup':None,'official_acceptance':'not determined by this script',
             'binaries':{k:{'file':v,'sha256':sha(v)} for k,v in {'cpu':a.cpu_binary,'gpu':a.gpu_binary,'worker':a.worker}.items()},
             'timing':'data imported and checked first; CREATE VECTOR INDEX through first real SQL query; no prebuilt index reused',
             'status':'running'}
    connections={}
    try:
        for name,port in [('cpu',a.cpu_port),('gpu',a.gpu_port)]:
            conn=pymysql.connect(host=a.host,port=port,user=a.user,password=os.getenv('SEEKDB_PASSWORD',''),
                                 autocommit=True,connect_timeout=15,read_timeout=1800,write_timeout=120)
            connections[name]=conn
            database='amd_paired_'+name+'_'+uuid.uuid4().hex[:10]
            with conn.cursor() as c:
                c.execute(f'CREATE DATABASE `{database}`');c.execute(f'USE `{database}`')
                c.execute('SELECT VERSION()');version=c.fetchone()[0]
                parameters={}
                for key in ('cpu_count','memory_limit','_memory_budget','vector_memory_limit'):
                    c.execute('SHOW PARAMETERS LIKE %s',(key,))
                    names=[x[0].lower() for x in c.description]
                    parameters[key]=[dict(zip(names,row)).get('value') for row in c.fetchall()]
            summary[name]={'database':database,'version':version,'server_parameters':parameters}
        if summary['cpu']['server_parameters']!=summary['gpu']['server_parameters']:
            raise RuntimeError('CPU/GPU server resource parameters differ')
        write_json(out/'summary.json',summary)
        for iteration in range(1,a.rounds+1):
            for name in (['cpu','gpu'] if iteration%2 else ['gpu','cpu']):
                log=Path(a.cpu_log if name=='cpu' else a.gpu_log)
                started=time.time()
                with log.open('rb') as stream:
                    inode=os.fstat(stream.fileno()).st_ino
                    stream.seek(0,2)
                    row=run_round(connections[name],name,iteration,config,base,queries,ids,truth,out)
                    fragment=stream.read().decode('utf-8',errors='replace')
                # Keep the old inode open across rotation, then add newly created log pieces.
                if log.stat().st_ino!=inode:
                    new_parts=sorted((x for x in log.parent.glob(log.name+'*') if x.is_file() and x.stat().st_ino!=inode and x.stat().st_mtime>=started),key=lambda x:x.stat().st_mtime)
                    fragment+='\n'.join(x.read_text(errors='replace') for x in new_parts)
                relevant=[line for line in fragment.splitlines() if 'HIPVS_' in line or row['table'] in line]
                evidence_file=out/(row['table']+'.construction.log')
                evidence_file.write_text('\n'.join(relevant)+'\n',encoding='utf-8')
                ready=re.findall(r'\[HIPVS_BUILD_READY\].*?rows=(\d+)',fragment)
                workers=re.findall(r'\[HIPVS_HNSW_BUILD\].*?rows=(\d+).*?fallback=0',fragment)
                failure='[HIPVS_BUILD_FAILED]' in fragment or '[HIPVS_HNSW_BUILD_FAILED]' in fragment or '[HIPVS_BUILD_FALLBACK]' in fragment
                valid=row['status']=='sql_smoke_passed' and row['metrics']['recall_at_k']>=a.min_recall
                first_valid=score_results([truth[0]],[{'query_id':truth[0]['query_id'],'neighbors':row.get('first_query',[])}],ids)['valid_results']
                valid=valid and first_valid
                ready_in_timed_window=False
                if name=='gpu':
                    ready_times=re.findall(r'\[([0-9-]+ [0-9:.]+)\][^\n]*\[HIPVS_BUILD_READY\]',fragment)
                    if len(ready_times)==1 and row.get('first_query_finished_utc'):
                        stamp=datetime.strptime(ready_times[0],'%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=timezone.utc)
                        ready_in_timed_window=datetime.fromisoformat(row['build_started_utc'])<=stamp<=datetime.fromisoformat(row['first_query_finished_utc'])
                    valid=valid and ready==[str(len(base))] and workers==[str(len(base))] and not failure and ready_in_timed_window
                if name=='cpu':valid=valid and not workers
                evidence={'table':row['table'],'backend':name,'iteration':iteration,'valid':bool(valid),
                          'seconds':row.get('observed_build_plus_first_query_seconds'),
                          'ddl_seconds':row.get('ddl_seconds'),'first_query_seconds':row.get('first_query_seconds'),
                          'recall':row.get('metrics',{}).get('recall_at_k'),
                          'gpu_build_rows':workers,'native_import_rows':ready,
                          'first_query_valid':first_valid,'gpu_ready_within_timed_window':ready_in_timed_window,
                          'construction_log_sha256':sha(evidence_file)}
                summary['rounds'].append(evidence)
                write_json(out/'summary.json',summary)
                if not valid:raise RuntimeError('round did not meet correctness, quality or GPU evidence checks: '+row['table'])
        medians={name:statistics.median(r['seconds'] for r in summary['rounds'] if r['backend']==name) for name in ('cpu','gpu')}
        summary.update(status='local_validation_passed',medians_seconds=medians,measured_speedup=medians['cpu']/medians['gpu'])
    except Exception as exc:
        summary.update(status='failed',error_type=type(exc).__name__)
        if isinstance(exc,RuntimeError):summary['error']=str(exc)
        write_json(out/'summary.json',summary)
        raise
    finally:
        for conn in connections.values():conn.close()
    write_json(out/'summary.json',summary)
    print(json.dumps({'medians':summary['medians_seconds'],'speedup':summary['measured_speedup'],'official_acceptance':summary['official_acceptance']}),flush=True)


if __name__=='__main__':main()
