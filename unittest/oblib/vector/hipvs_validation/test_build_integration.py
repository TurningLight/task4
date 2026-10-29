"""Linux build wiring/runtime discovery checks; fake hipcc is NOT GPU validation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[4]
CMAKE=os.environ.get('CMAKE','cmake')
CXX=os.environ.get('CXX','g++')


def command(args,**kwargs):
    return subprocess.run([str(a) for a in args],capture_output=True,text=True,**kwargs)


class BuildIntegration(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='hipvs-build-')
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.source=self.root/'source'
        self.source.mkdir()
        self.build=self.root/'build'
        self.hipvs=self.root/'hipvs'
        self.rocm=self.root/'rocm'
        self.bridge=self.root/'libseekdb_cuvs_bridge.so'
        (self.source/'main.cpp').write_text('int main() { return 0; }\n')
        (self.source/'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
include("{ROOT.as_posix()}/cmake/Utils.cmake")
include("{ROOT.as_posix()}/cmake/HipvsConfig.cmake")
project(hipvs_build_wiring LANGUAGES CXX)
add_executable(seekdb main.cpp)
set_target_properties(seekdb PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/src/observer")
install(TARGETS seekdb RUNTIME DESTINATION bin)
include("{ROOT.as_posix()}/cmake/HipvsWorker.cmake")
''')
        target=self.source/'tools/hipvs_bridge'
        target.mkdir(parents=True)
        for name in ('build_hnsw_worker.sh','hnsw_build_worker.cpp','hnsw_hierarchy.h'):
            shutil.copyfile(ROOT/'tools/hipvs_bridge'/name,target/name)

    def full_dependencies(self):
        for name in ('include/cuvs/core/c_api.h','include/cuvs/neighbors/cagra.h',
                     'include/hnswlib/hnswlib.h','lib/libcuvs_c.so'):
            p=self.hipvs/name;p.parent.mkdir(parents=True,exist_ok=True);p.touch()
        self.bridge.touch()
        compiler=self.rocm/'bin/hipcc'
        compiler.parent.mkdir(parents=True)
        compiler.write_text('''#!/bin/sh
set -eu
out=''
while [ "$#" -gt 0 ]; do
  if [ "$1" = -o ]; then shift; out=$1; fi
  shift
done
test -n "$out"
printf '#!/bin/sh\\nexit 0\\n' > "$out"
chmod +x "$out"
''')
        compiler.chmod(0o755)

    def configure(self,*options):
        return command([CMAKE,'-S',self.source,'-B',self.build,
                        '-DSEEKDB_HIPVS_ROOT='+str(self.hipvs),
                        '-DSEEKDB_ROCM_ROOT='+str(self.rocm),
                        '-DCUVS_BRIDGE_LIB='+str(self.bridge),*options])

    def test_no_gpu_dependencies_keeps_cpu_build(self):
        p=self.configure();self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('enabled=OFF; worker=OFF',p.stdout)
        p=command([CMAKE,'--build',self.build,'--target','seekdb'])
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertFalse((self.build/'src/observer/seekdb-hipvs-hnsw-build').exists())

    def test_image_dependencies_build_worker_with_seekdb_and_install(self):
        self.full_dependencies()
        p=self.configure();self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('enabled=ON; worker=ON; offline=ON',p.stdout)
        p=command([CMAKE,'--build',self.build,'--target','seekdb'])
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        worker=self.build/'src/observer/seekdb-hipvs-hnsw-build'
        self.assertTrue(os.access(worker,os.X_OK))
        worker.unlink()
        p=command([CMAKE,'--build',self.build,'--target','seekdb'])
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertTrue(worker.exists(),'make seekdb must regenerate a missing worker')
        installed=self.root/'installed'
        p=command([CMAKE,'--install',self.build,'--prefix',installed])
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertTrue(os.access(installed/'bin/seekdb-hipvs-hnsw-build',os.X_OK))

    def test_explicit_off_overrides_image_detection(self):
        self.full_dependencies()
        p=self.configure('-DOB_BUILD_CUVS=OFF')
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertIn('enabled=OFF; worker=OFF',p.stdout)

    def test_explicit_on_without_bridge_fails_clearly(self):
        p=self.configure('-DOB_BUILD_CUVS=ON')
        self.assertNotEqual(p.returncode,0)
        self.assertIn('requires an existing CUVS_BRIDGE_LIB',p.stderr)


class WorkerDiscovery(unittest.TestCase):
    def test_relocation_overrides_and_no_cwd_search(self):
        with tempfile.TemporaryDirectory(prefix='hipvs-discovery-') as name:
            root=Path(name)
            source=root/'probe.cpp'
            source.write_text('#include "hipvs_bulk_builder.h"\n#include <iostream>\n'
                'int main(){std::cout << oceanbase::common::obvsag::HipvsBulkBuilder::find_worker();}\n')
            binary=root/'seekdb'
            p=command([CXX,'-std=c++17','-pthread','-I',ROOT/'src/oblib/lib/vector',
                       source,ROOT/'src/oblib/lib/vector/hipvs_bulk_builder.cpp','-o',binary])
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            env=dict(os.environ);env.pop('SEEKDB_HIPVS_BUILD_WORKER',None)
            def resolved(exe=binary,extra=None):
                p=command([exe],env={**env,**(extra or {})},cwd='/')
                self.assertEqual(p.returncode,0,p.stderr)
                return p.stdout
            self.assertEqual(resolved(),'')
            worker=root/'seekdb-hipvs-hnsw-build';worker.write_text('#!/bin/sh\nexit 0\n')
            self.assertEqual(resolved(),'','nonexecutable file is not a worker')
            worker.chmod(0o755)
            self.assertEqual(resolved(),str(worker))
            self.assertEqual(resolved(extra={'SEEKDB_HIPVS_BUILD_WORKER':''}),'')
            self.assertEqual(resolved(extra={'SEEKDB_HIPVS_BUILD_WORKER':'/explicit/worker'}),'/explicit/worker')
            relocated=root/'relocated';relocated.mkdir()
            binary.rename(relocated/'seekdb');worker.rename(relocated/worker.name)
            self.assertEqual(resolved(relocated/'seekdb'),str(relocated/worker.name))


if __name__=='__main__':unittest.main(verbosity=2)
