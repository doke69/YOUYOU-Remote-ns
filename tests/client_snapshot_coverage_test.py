"""Ensure the UI cache test covers every declared Snapshot member."""
from pathlib import Path
import re

root=Path(__file__).resolve().parents[1]
header=(root/'include/client_snapshot.hpp').read_text(encoding='utf-8')
body=header.split('struct Snapshot {',1)[1].split('    // UI owns',1)[0]
body=re.sub(r'//[^\n]*','',body)
members=[]
for statement in body.split(';'):
    statement=statement.strip()
    if not statement:continue
    declaration=re.fullmatch(r'(bool|std::string|PrivacyMode|Json|uint64_t|int64_t|int)\s+([\s\S]+)',statement)
    assert declaration,statement
    for item in re.split(r',(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)',declaration[2]):
        members.append(re.match(r'\s*(\w+)',item)[1])
test=(root/'tests/client_snapshot_test.cpp').read_text(encoding='utf-8')
tested=re.findall(r'^\s+FIELD\((\w+),',test,re.M)
assert set(tested)==set(members)-{'devices'},(set(members)-set(tested),set(tested)-set(members))
assert len(tested)==len(set(tested))
client=(root/'source/client.cpp').read_text(encoding='utf-8')
# These are the two owners of the JSON list. Existing UI selection caching also
# depends on this revision. Keep the update adjacent to the assignment.
writes=re.findall(r'state_\.devices\s*=.*?;',client)
assert len(writes)==2
for write in writes:assert write+'\n'+('            ' if 'Json::array()' in write else '        ')+'++state_.devicesRevision;' in client
print(f'PASS: {len(members)} members covered; both device-list writes advance revision')
