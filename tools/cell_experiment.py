"""Run bounded P4 capture comparisons through the same worker used by Auto."""
import argparse
import json
import re
import time
from pathlib import Path
from cell_console import Console


def run(console, hz, rate, ms, destination, verify_raw=False):
    before=console.command('celliq status')
    if 'CELLIQ busy=0' not in before:
        raise RuntimeError('An existing capture is active; leave its session alone')
    response=console.command(f'celliq once {hz} {rate} {ms}')
    if 'CELLIQ ONCE started' not in response:
        raise RuntimeError(response)
    deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        time.sleep(1)
        response=console.command('celliq status')
        if 'CELLIQ busy=0' in response:
            break
    else:
        console.command('celliq stop')
        raise TimeoutError('Capture did not finish within 90 seconds')
    name=re.search(r'CELLIQ saved=(hrf_[A-Za-z0-9_.]+\.cu8)',response)
    if not name:
        raise RuntimeError('No observation filename: '+response)
    data=console.download(name[1]+'.json',destination/(name[1]+'.json'))
    result=json.loads(data)
    if verify_raw:
        if not result['raw_saved']:
            raise RuntimeError('Cannot verify an observation without saved raw IQ')
        result['sd_readback_verified']=console.check_file(name[1],result['bytes'],result['crc32'])
    print(json.dumps(dict(file=name[1],rate=result['rate'],complete=result['complete'],
        elapsed_us=result['capture_elapsed_us'],host_drops=result['dropped'],
        radio_health=result['radio_health'],sync=result['lte']['found'],
        mib=result['lte']['mib']['found'],analysis_ms=result['lte']['analysis_ms'],
        profile=result.get('profile'),sd_readback_verified=result.get('sd_readback_verified'))),flush=True)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--hz',type=int,default=739000000)
    parser.add_argument('--rates',type=int,nargs='+',default=[8000000,10000000])
    parser.add_argument('--ms',type=int,default=80)
    parser.add_argument('--repeats',type=int,default=2)
    parser.add_argument('--verify-raw',action='store_true',help='Verify SD bytes/CRC on device; requires celliq check support')
    args=parser.parse_args()
    if not 1<=args.repeats<=10 or len(args.rates)>6:
        parser.error('Use 1-10 repetitions and at most six rates')
    args.out.mkdir(parents=True,exist_ok=True)
    console=Console(args.port)
    results=[]
    try:
        mode=console.command('cellperf')
        if 'CELL PERFORMANCE: HIGH RATE' not in mode:
            raise RuntimeError('Enter performance mode first (cellperf on)')
        for _ in range(args.repeats):
            for rate in args.rates:
                results.append(run(console,args.hz,rate,args.ms,args.out,args.verify_raw))
    finally:
        console.close()
        (args.out/'results.json').write_text(json.dumps(results,indent=2))


if __name__=='__main__':main()
