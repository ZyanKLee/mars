#!/bin/bash
# Copyright 2010-2013 Frank Liepold /  1&1 Internet AG
#
# Email: frank.liepold@1und1.de
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#####################################################################

# This script is used to generate a given rate of write operations on a 
# given number of mars resources.
# It is assumed that the data devices are named as follows (the strings
# in <...> denotes shell variables defined below):
# /dev/mars/lv-1-<dev_size_gb> ... /dev/mars/lv-<max_res_nr>-<dev_size_gb>
#
# The given write rate kb_per_sec is put into effect as follows:
# 1.) at first kb_per_sec is divided in max_res_nr random defined
#     percentage write rates so that the sum of these
#     rates amount to kb_per_sec
# 2.) for each of these percentage rates > 0 a write process is started
#     which realizes this rate on one resource.
# 3.) after a random defined time between min_runtime_writer and
#     max_runtime_writer the processes are killed and we restart at 1.)

# number of resources
max_res_nr=7

# min- and maxtime (seconds) the started write processes may write on the data
# devices before they are killed
max_runtime_writer=60
min_runtime_writer=10

# max value of $RANDOM
max_random=32767

# write rate in KB/s 
kb_per_sec=70000 # corresponds to 4G / 10min

# size of the data device in GB
dev_size_gb=2

# size of the data device in KB
device_size_kb=$(($dev_size_gb * 1024 * 1024))

# time (seconds) after which log-rotate and log-delete should be called
log_rotate_delete_intervall=600

function calculate_kb_per_sec_per_resource
{
    local i percentage_left=100
    for i in $(seq 1 1 $max_res_nr); do
        if [ $percentage_left -gt 0 ]; then
            percentage[$i]=$(( $RANDOM * $percentage_left / $max_random ))
            let percentage_left-=${percentage[$i]}
        else
            percentage[$i]=0
        fi
        kb_per_sec_res[$i]=$(( $kb_per_sec * ${percentage[$i]} / 100))
    done
    date && echo kb_per_sec_res="${kb_per_sec_res[@]}"
}

function start_writer_on_resource
{
    local res_nr=$1
    local kb_per_sec=${kb_per_sec_res[$i]}
    local output_dev=/dev/mars/lv-$res_nr-$dev_size_gb
    local loop_count=0
    date && echo "starting writer on $res_nr (kb_per_sec=$kb_per_sec)"
    if [ $kb_per_sec -eq 0 ]; then
        sleep 10 # because caller checks whether I'm running
        exit 0
    fi
    while true; do
        local maximal_offset_output=$(($device_size_kb - (2 * $kb_per_sec) ))
        local offset_output=$(( $RANDOM * $maximal_offset_output / $max_random ))
        local dd_cmd="dd of=$output_dev bs=1024 count=$kb_per_sec skip=$offset_output"
        date && echo "executing on res $res_nr(loop=$loop_count): $dd_cmd"
        yes hallo | $dd_cmd
        sleep 1
        let loop_count+=1
        if [ $loop_count -ge $max_runtime_writer ]; then
            exit 0
        fi
    done
}

function check_process_running
{
    local pid=$1 res_nr=$2 maxwait=5 waited=0
    date && echo "check process $pid for resource $res_nr"
    while true; do
        if ps -p $pid; then
            break
        fi
        sleep 1
        let waited+=1
        date && echo "waited $waited for pid $pid"
        if [ $waited -eq $maxwait ]; then
            date && echo "maxwait $maxwait exceeded" >&2 && exit 1
        fi
    done
}

function kill_process
{
    local pid=$1 res_nr=$2
    local signal
    date && echo "killing process $pid for resource $res_nr"
    for signal in 1 9 "check"; do
        if ps -p $pid; then
            if [ "$signal" != "check" ]; then
                kill -$signal $pid
                sleep 1
            else
                echo "could not kill pid $pid" >&2 && exit 1
            fi
        else
            return
        fi
    done
}


sum_sleeptime=0

while true; do
    calculate_kb_per_sec_per_resource
    runtime_writer=$(( ($RANDOM * ($max_runtime_writer - $min_runtime_writer) / $max_random ) + $min_runtime_writer))
    date && echo runtime_writer=$runtime_writer
    for i in $(seq 1 1 $max_res_nr); do
        tmp_file=/tmp/dd.$$.$i
        rm -f $tmp_file || exit 1
        start_writer_on_resource $i >$tmp_file 2>&1 &
        pid[$i]=$!
        check_process_running ${pid[$i]} $i
    done
    date && echo sleeping $runtime_writer
    sleep $runtime_writer
    let sum_sleeptime+=$runtime_writer
    for i in $(seq 1 1 $max_res_nr); do
        kill_process ${pid[$i]} $i
    done
    if [ $sum_sleeptime -ge $log_rotate_delete_intervall ]; then
        marsadm log-rotate all
        marsadm log-delete all
        sum_sleeptime=0
    fi
done

