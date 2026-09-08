"""Real installed refusal and codec-thread/sink lifetime controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time

from encodec_transport import run_case, stop


def sink(info, destination):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    descriptors={}
    for name in os.listdir('/proc/self/fd'):
        try:descriptors[name]=os.readlink('/proc/self/fd/'+name)
        except FileNotFoundError:pass
    Path(info).write_text(json.dumps({'pid':os.getpid(),'descriptors':descriptors}))
    with Path(destination).open('wb') as output:
        while block:=os.read(0,65536):
            output.write(block);output.flush()
    # Closing stdin and TERM deliberately do not finish this owned sink.
    while True:time.sleep(1)


def population(root):
    result={}
    for path in sorted(root.rglob('*')):
        if path.is_file():
            with path.open('rb') as source:digest=hashlib.file_digest(source,'sha256').hexdigest()
            result[str(path.relative_to(root))]=digest
    return result


def main():
    if len(sys.argv)==4 and sys.argv[1]=='--sink':
        sink(sys.argv[2],sys.argv[3]);return 0
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--new-bin',type=Path,required=True)
    parser.add_argument('--assets',type=Path,required=True)
    parser.add_argument('--content-root',type=Path,required=True)
    parser.add_argument('--state',type=Path,required=True)
    parser.add_argument('--evidence',type=Path,required=True)
    options=parser.parse_args();options.old_bin=None
    options.evidence.mkdir(parents=True,exist_ok=False)
    before={'content':population(options.content_root),'state':population(options.state)}
    os.environ.update(KILIX_CONTENT_ROOT=str(options.content_root),XDG_STATE_HOME=str(options.state),
                      KILIX_ENCODEC_24KHZ_DIR=str(options.assets),KILIX_ENCODEC_48KHZ_DIR=str(options.assets),
                      HOME=str(options.evidence/'untrusted-home'))
    rows=[run_case(options,'authority',6,48000,2,installed_fallback=True)]
    for root in (str(options.content_root),'relative-content-root'):
        environment=os.environ.copy();environment['KILIX_CONTENT_ROOT']=root
        command=[str(options.new_bin/'kmx-attach'),'--socket',str(options.evidence/'absent.sock'),
                 '--audio-codec','encodec','--dump']
        result=subprocess.run(command,env=environment,capture_output=True,timeout=5)
        label='actual' if root==str(options.content_root) else 'relative'
        (options.evidence/(label+'-forced-refusal.stderr')).write_bytes(result.stderr)
        assert result.returncode==1 and b'EnCodec unavailable; explicit selection refused' in result.stderr
        assert b'connect:' not in result.stderr
        rows.append({'case':label+' forced pre-connect refusal','passed':True})
    child=subprocess.Popen(['/bin/sleep','60'],start_new_session=True)
    try:
        directory=options.evidence/'sink';info=directory/'sink-info.json'
        command='exec '+shlex.join([sys.executable,str(Path(__file__).resolve()),'--sink',str(info),str(directory/'received.pcm')])
        result=run_case(options,'sink',6,24000,1,sink_command=command)
        record=json.loads(info.read_text())
        assert set(record['descriptors'])=={'0','1','2'},record
        assert record['descriptors']['0'].startswith('pipe:') and record['descriptors']['1']==record['descriptors']['2']=='/dev/null'
        assert not Path('/proc/'+str(record['pid'])).exists()
        assert child.poll() is None
        assert result['client_exit']==0 and result['client_shutdown_seconds']<3
        rows.append(result)
    finally:stop(child)
    assert before=={'content':population(options.content_root),'state':population(options.state)}
    (options.evidence/'result.json').write_text(json.dumps({'all_passed':True,'rows':rows,
        'original_content_and_receipts_unchanged':True,'normal_mono_admission':False,
        'synthetic_sink_private_stdio_only':True,'unrelated_child_preserved':True,'input_hashes':before},indent=2)+'\n')
    print(json.dumps({'passed_cases':len(rows),'original_files':sum(map(len,before.values()))}))
    return 0


if __name__=='__main__':raise SystemExit(main())
