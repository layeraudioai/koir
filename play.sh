echo "YOU are a fool!"
read -n 1

if [[ "${1}" != " " ]]; 
then 
SONGS=();
idx=0;
shopt -s nullglob;
SONGS+=("${1}");
shopt -u nullglob;
SELECTED_FILE="${SONGS[0]}";
else
echo "Available songs:";
SONGS=();
idx=0;
shopt -s nullglob;
for f in compiles/*; do
    SONGS+=("$f");
    echo "  [$idx] $f";
    ((idx++));
done;
shopt -u nullglob;
read -p "Select song [0-$((idx-1))] (default 0): " s_idx;
s_idx=${s_idx:-0};
SELECTED_FILE="${SONGS[$s_idx]}";
1=$SELECTED_FILE
fi;

while ((compiles/*_*_*_*"$1"* && clear)&); do clear; read -n 1 quit; if !(($((${quit})))); then break; fi; clear; done; clear;
