# Copyright (c) 2023 Jamie M.
#
# All right reserved. This code is not apache or FOSS/copyleft licensed.

import serial
import io
ser = serial.Serial('/dev/ttyACM0', 115200, timeout=2)
ser.write(b"ess readings\r\n")
rec = ser.read_until(expected=b"\n", size=None)

ser.close()

start_delim = rec.find(b"##", 0)
end_delim = rec.find(b"^^", start_delim)

rec = rec[(start_delim+2):end_delim]
rec = rec.split(b",")

vals = len(rec) / 5
i = 0

if (vals > 0):
    while (i < vals):
        idx = rec[5*i].decode("utf-8")
        temperature = rec[(5*i)+1].decode("utf-8")
        pressure = rec[(5*i)+2].decode("utf-8")
        humidity = rec[(5*i)+3].decode("utf-8")
        dew_point = rec[(5*i)+4].decode("utf-8")
        print("sensor" + idx + ",temperature=" + temperature + ",pressure=" + pressure + ",humidity=" + humidity + ",dew_point=" + dew_point)

        i = i + 1
