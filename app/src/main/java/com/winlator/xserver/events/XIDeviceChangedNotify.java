package com.winlator.xserver.events;

import com.winlator.xconnector.XOutputStream;
import com.winlator.xconnector.XStreamLock;
import com.winlator.xserver.extensions.XInput2Extension;

import java.io.IOException;

/**
 * XInput2 XI_DeviceChanged event (xXIDeviceChangedEvent from XI2proto.h).
 *
 * Sent when the master pointer's advertised classes change - specifically when
 * native touch mode is toggled at runtime, which flips the X/Y valuators
 * between Relative and Absolute mode. wine's winex11.drv selects
 * XI_DeviceChanged on the root window and calls update_relative_valuators()
 * with the classes carried by this event, so emitting it lets wine pick up
 * the new valuator mode without a restart.
 *
 * Wire format (32-byte XGE header, then the same class structs used in the
 * XIQueryDevice reply):
 *
 *   uint8_t     type;         // Always GenericEvent (35)
 *   uint8_t     extension;    // XInput extension major opcode
 *   uint16_t    sequenceNumber;
 *   uint32_t    length;       // Length of trailing payload in 4-byte units
 *   uint16_t    evtype;       // XI_DeviceChanged (1)
 *   uint16_t    deviceid;
 *   Time        time;
 *   uint16_t    num_classes;
 *   uint16_t    sourceid;
 *   uint8_t     reason;       // XIDeviceChange (2)
 *   uint8_t     pad0;
 *   uint16_t    pad1;
 *   uint32_t    pad2;
 *   uint32_t    pad3;
 */
public class XIDeviceChangedNotify extends Event {
    public static final int GENERIC_EVENT_CODE = 35;
    private static final short XI_DEVICE_CHANGED_EVTYPE = 1;
    private static final byte REASON_DEVICE_CHANGE = 2;

    private final byte extensionOpcode;
    private final int deviceId;
    private final int numButtons;
    private final boolean absoluteValuators;
    private final int maxX;
    private final int maxY;

    public XIDeviceChangedNotify(int deviceId, byte extensionOpcode, int numButtons,
                                 boolean absoluteValuators, int maxX, int maxY) {
        super(GENERIC_EVENT_CODE);
        this.deviceId = deviceId;
        this.extensionOpcode = extensionOpcode;
        this.numButtons = numButtons;
        this.absoluteValuators = absoluteValuators;
        this.maxX = maxX;
        this.maxY = maxY;
    }

    @Override
    public void send(short sequenceNumber, XOutputStream outputStream) throws IOException {
        try (XStreamLock lock = outputStream.lock()) {
            final int numValuators = 2;
            final int numClasses = 1 + numValuators; // button class + X/Y valuators

            int payloadBytes = XInput2Extension.buttonClassBytes(numButtons)
                    + (XInput2Extension.VALUATOR_CLASS_BYTES * numValuators);
            int payloadLengthUnits = payloadBytes / 4;

            outputStream.writeByte(this.code);                        // [0] type = 35
            outputStream.writeByte(extensionOpcode);                  // [1] extension opcode
            outputStream.writeShort(sequenceNumber);                  // [2-3] sequence number
            outputStream.writeInt(payloadLengthUnits);                // [4-7] payload length
            outputStream.writeShort(XI_DEVICE_CHANGED_EVTYPE);        // [8-9] evtype = 1
            outputStream.writeShort((short) deviceId);                // [10-11] deviceid
            outputStream.writeInt((int) System.currentTimeMillis()); // [12-15] time
            outputStream.writeShort((short) numClasses);              // [16-17] num_classes
            outputStream.writeShort((short) deviceId);                // [18-19] sourceid
            outputStream.writeByte(REASON_DEVICE_CHANGE);             // [20] reason
            outputStream.writePad(11);                                // [21-31] pad0/pad1/pad2/pad3

            XInput2Extension.writeButtonClass(outputStream, deviceId, numButtons);
            XInput2Extension.writeValuatorClass(outputStream, deviceId, 0, absoluteValuators, maxX);
            XInput2Extension.writeValuatorClass(outputStream, deviceId, 1, absoluteValuators, maxY);
        }
    }
}
