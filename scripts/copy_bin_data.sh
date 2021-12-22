#!/bin/bash
# Copy binary files containing data from data_generator to the other machine 
ip_flag='192.168.2.2'

print_usage() {
  printf "Usage: sh copy_bin_data.sh -a <IP Address> \n"
}

while getopts 'a:' flag; do
  case "${flag}" in
    a) ip_flag="${OPTARG}" ;;
    *) print_usage
       exit 1 ;;
  esac
done
echo "Copying ../data/bin folder to" $ip_flag":/scratch/repos/agora/data/"
scp -r ../data/bin root@$ip_flag:/scratch/repos/agora/data/
