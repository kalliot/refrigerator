#!/usr/bin/env python


import os
import json
import time
import sys
from datetime import datetime
from queue import Queue, Empty
import paho.mqtt.client as mqtt 



def processData(data, filelen):

    if data['id'] == "otastatus":
        len = int(data['value'])
        if len==0:
            print(datetime.now(), "Done")
            exit()
            
        percentage = int(100 * len / filelen)
        print(percentage,"%",end='\r')

# home/kallio/refrigerator/${DEVICE}/otaupdate
# {\"dev\":\"${DEVICE}\",\"id\":\"otaupdate\",\"file\":\"${FNAME}\"}

def startTransfer(device, file):
    global lastRead

    topic = "home/kallio/refrigerator/" + device + "/otaupdate"
    now = int(time.time())
    startmsg = {
        'dev': device,
        'id': 'otaupdate',
        'ts': now,
        'file': file
    }    
    client.publish(topic, json.dumps(startmsg), qos=0, retain=False) 


def on_message(client, userdata, message):
    msgqueue.put(message.payload)


#def on_connect(client, userdata, flags, rc, properties):
def on_connect(client, userdata, flags, rc):
    global connected
    if (rc==0):
        connected = True
        topics = [("home/kallio/refrigerator/+/otaupdate",0)]
        client.subscribe(topics)

    else:
        print(datetime.now(),"connection failed, rc=",rc)
        connected = False


def on_disconnect(client, userdata, rc):
    print(datetime.now(),"disconnected")


if len(sys.argv) < 4:
    print(sys.argv[0], "dev filename filesize")
    exit()

dev = sys.argv[1]
fname = sys.argv[2]
fsize = int(sys.argv[3])

msgqueue = Queue(maxsize=10)


client = mqtt.Client("otastatus" + str(os.getpid()))
#client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2),"otastatus" + str(os.getpid())
client.on_connect = on_connect
client.on_message = on_message
client.on_disconnect = on_disconnect

client.connect("192.168.101.231", 1883, 60)
print(datetime.now(), "Start")
client.loop_start()

startTransfer(dev, fname)


while True:
    try:
        data = msgqueue.get(block=True, timeout=15)
        if data is None:
            continue
        msg = json.loads(data)
        processData(msg, fsize)

    except Empty:
        print("timeout")
        exit()

    except KeyboardInterrupt:
        client.disconnect()
        exit(0)

    except json.decoder.JSONDecodeError as e:
        print(datetime.now(), "json:", e)
        print(datetime.now(), data)

    except:
        raise        
