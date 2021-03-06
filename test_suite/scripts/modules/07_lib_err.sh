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


function lib_err_check_nonexistence_of_other_error_messages
{
    local host=$1 msg_file=$2 errmsg_pattern=$3
    local rc
    lib_vmsg "  checking non existence of $errmsg_pattern in $host@$msg_file"
    lib_remote_idfile $host \
                      "egrep '$main_mars_errmsg_prefix' $msg_file | egrep -v '$errmsg_pattern'"
    rc=$?
    if [ $rc -eq 0 ];then
        lib_exit 1 "other errors than $errmsg_pattern found in $host@$msg_file"
    fi
}
    

# we search for lines like the following:
#
# Sep 25 20:07:35 istore-test-bs7 kernel: : Stack:
#
# extract the date and compare it with main_start_time

function lib_check_for_kernel_oops_after_start_time
{
    local host last_stack_line kern_log=/var/log/kern.log
    if [ -z "$main_start_time" ];then
        echo "  variable main_start_time not set" >&2
        echo "  cannot look for recent kernel oops" >&2
        return
    fi
    for host in "${main_host_list[@]}"; do
        last_stack_line="$(lib_remote_idfile $host "grep -w Stack $kern_log | tail -1")"
        if [ -n "$last_stack_line" ]; then
            lib_vmsg "  last kernel stack on $host: $last_stack_line"
            local stack_date=$(date -d "$(echo $last_stack_line | \
                                        awk '{print $1,$2,$3}')" +'%Y%m%d%H%M%S')
            if [ $stack_date -gt $main_start_time ]; then
                lib_remote_idfile $host "cp -f $kern_log $kern_log.$main_start_time"
                echo "KERNEL-STACK on $host at $stack_date. Saved in $kern_log.$main_start_time"
            fi
        fi
    done
}


function lib_general_checks_after_every_test
{
	lib_err_check_and_move_global_err_files_all
	lib_check_proc_sys_mars_variables
    lib_check_for_kernel_oops_after_start_time
}

function lib_check_proc_sys_mars_variables
{
    local host rc
    local file_pattern='/proc/sys/mars/mem*'
    for host in "${main_host_list[@]}"; do
	lib_vmsg "  displaying $file_pattern on $host"
	lib_remote_idfile $host 'if ls -lrt '"$file_pattern"';then for f in '"$file_pattern"';do echo $f; cat $f;done;fi'
    done
}

function lib_err_check_and_move_global_err_files_all
{
    local host rc
    for host in "${main_host_list[@]}"; do
        lib_remote_idfile $host "test -s $lib_err_total_err_file"
        rc=$?
        if [ $rc -eq 0 ]; then
            local err_sav=$lib_err_total_err_file.$(date +'%Y%m%d%H%M%S')
            local log_sav=$lib_err_total_log_file$(date +'%Y%m%d%H%M%S')
            echo "ERROR-FILE $host:$lib_err_total_err_file (marsadm cat):" >&2
            lib_remote_idfile $host "marsadm cat $lib_err_total_err_file"
            lib_vmsg "  moving $lib_err_total_err_file to $err_sav"
            lib_remote_idfile $host "mv $lib_err_total_err_file $err_sav"
            lib_vmsg "  marsadm cat $lib_err_total_log_file > $log_sav"
            lib_remote_idfile $host \
                            "marsadm cat $lib_err_total_log_file > $log_sav"
        fi
    done
}

function lib_err_wait_for_error_messages
{
    [ $# -eq 5 ] || lib_exit 1 "wrong number $# of arguments (args = $*)"
    local host=$1 msg_file=$2 errmsg_pattern="$3"
    local number_errmsg_req=$4 maxwait=$5
    local count waited=0 rc

    lib_vmsg "  checking existence of file $msg_file on $host"
    lib_remote_idfile $host "ls -l --full-time $msg_file" || lib_exit 1
    while true; do
        count=$(lib_remote_idfile $host \
                "egrep '$errmsg_pattern' $msg_file | wc -l") || lib_exit 1
        lib_vmsg "  found $count messages (pattern = '$errmsg_pattern'), waited $waited"
        if [ $count -ge $number_errmsg_req ]; then
            break
        fi
        let waited+=1
        sleep 1
        if [ $waited -ge $maxwait ]; then
            lib_exit 1 "maxwait $maxwait exceeded"
        fi
    done
}

