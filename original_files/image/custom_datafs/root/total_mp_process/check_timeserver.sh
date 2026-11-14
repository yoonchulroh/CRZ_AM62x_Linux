#!/bin/bash

PRC_FOLDER_NAME=/data/root/total_mp_process/

echo timeout 3 rdate -s time.bora.net
timeout 3 rdate -s time.bora.net
if [ "$?" != 0 ]; then
    echo timeout 3 rdate -s zero.bora.net
    timeout 3 rdate -s zero.bora.net
    if [ "$?" != 0 ]; then
        echo timeout 3 rdate -s time.nist.gov
        timeout 3 rdate -s time.nist.gov
        if [ "$?" != 0 ]; then
            echo "fail"
#            $PRC_FOLDER_NAME/test_app_qt.sh notok Time_Server_Connect_Fail
            exit 1
        fi
    fi
fi

echo hwclock -u --systohc
hwclock -u --systohc
echo hwclock -u --systohc
hwclock -u --systohc

echo sync
sync

if [ "$?" == 0 ]; then
    echo "success"
    exit 0
else
    echo "fail"
#    $PRC_FOLDER_NAME/test_app_qt.sh notok Time_Setting_Fail
    exit 1
fi
