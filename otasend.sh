#!/bin/sh

# In computer 192.168.101.233, there is a
# openssl server running. It is starte with command
# openssl s_server -WWW -key ca_key.pem -cert ca_cert.pem -port 8070
# *.pem files are in same directory.
# sensors_x.x.x.x is copied to servers ota subdirectory.

# see also CONFIG_APP_PROJECT_VER and CONFIG_FIRMWARE_UPGRADE_URL
# in sdkconfig.

# fd9030 refrigerator
# 5bcae4 boiler
# 5bc674 dev board

DEVICE="fd9030"

cat sdkconfig | grep 'CONFIG_APP_PROJECT_VER=' > vernum.tmp
. ./vernum.tmp
FNAME="refrigerator_$CONFIG_APP_PROJECT_VER"
message='{"id":"otafileschanged","name":'\"${FNAME}\"'}'
echo $FNAME
sftp pi@192.168.101.233 << EOF
cd srv/ota
put build/refrigerator.bin $FNAME
EOF
FILESIZE="$(stat -c%s build/refrigerator.bin)"
mosquitto_pub -h 192.168.101.231 -t 'home/kallio/otafileschanged' -m $message

