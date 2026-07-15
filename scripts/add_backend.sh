#!/bin/bash
# Usage: add_backend.sh <ip> <port> [sim_mode] [config_path]
IP=${1:?ip required}
PORT=${2:?port required}
SIM=${3:-""}
CFG=${4:-config/config.json}
python3 -c "
import json, sys
with open('$CFG') as f: c = json.load(f)
ids = [b['id'] for b in c['backends']]
nid = max(ids)+1 if ids else 1
entry = {'id': nid, 'ip': '$IP', 'port': int('$PORT')}
if '$SIM': entry['sim_mode'] = '$SIM'
c['backends'].append(entry)
with open('$CFG','w') as f: json.dump(c, f, indent=2)
print(f'Added backend {nid} at $IP:$PORT')
"
