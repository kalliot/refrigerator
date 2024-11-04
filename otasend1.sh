#!/bin/sh

# In computer 192.168.101.233, there is a
# openssl server running. It is starte with command
# openssl s_server -WWW -key ca_key.pem -cert ca_cert.pem -port 8070
# *.pem files are in same directory.
# refrigerator_x.x.x.x is copied to servers ota subdirectory.

# see also CONFIG_APP_PROJECT_VER and CONFIG_FIRMWARE_UPGRADE_URL
# in sdkconfig.

cat sdkconfig | grep CONFIG_APP_PROJECT_VER > vernum.tmp
. ./vernum.tmp
echo $CONFIG_APP_PROJECT_VER
FNAME="refrigerator_$CONFIG_APP_PROJECT_VER"
echo $FNAME
message='{"dev":"fd9030","id":"otaupdate","file":'\"${FNAME}\"'}'
echo $message
sftp pi@192.168.101.233 << EOF
cd srv/ota
put build/refrigerator.bin $FNAME
EOF
# mqtt message is a signal for the running esp prog, to start ota update.
mosquitto_pub -h 192.168.101.231 -t home/kallio/refrigerator/fd9030/otaupdate -m $message
echo 'DONE'
