package com.winlator.xserver.events;

import com.winlator.xconnector.XOutputStream;
import com.winlator.xconnector.XStreamLock;

import java.io.IOException;

/**
 * XInput2 raw touch event (XI_RawTouchBegin / XI_RawTouchUpdate / XI_RawTouchEnd).
 *
 * Wire format is xXIRawEvent from XI2proto.h (identical layout to the raw
 * motion/button events already emitted by XIRawMotionNotify), sent as an
 * X Generic Event (type 35):
 *
 *   uint8_t     type;           // Always GenericEvent (35)
 *   uint8_t     extension;      // XInput extension major opcode
 *   uint16_t    sequenceNumber;
 *   uint32_t    length;         // Length of trailing payload in 4-byte units
 *   uint16_t    evtype;         // 22/23/24 = RawTouchBegin/Update/End
 *   uint16_t    deviceid;
 *   Time        time;
 *   uint32_t    detail;         // Touch sequence ID
 *   uint16_t    sourceid;
 *   uint16_t    valuators_len;  // Length of trailing valuator mask in 4-byte units
 *   uint32_t    flags;
 *   uint32_t    pad2;
 *   // payload: mask, axisvalues (FP3232[]), axisvalues_raw (FP3232[])
 *
 * wine's winex11.drv X11DRV_RawTouchEvent consumes these and translates them
 * into WM_POINTERDOWN/UPDATE/UP hardware input. It requires:
 *  - deviceid to match the master pointer returned by XIGetClientPointer
 *  - both X (0) and Y (1) valuators present in every event
 *  - the device's valuator classes to be advertised as Absolute mode
 *    (see XInput2Extension.writeValuatorClass)
 */
public class XIRawTouchNotify extends Event {
    public static final int GENERIC_EVENT_CODE = 35;

    private final byte extensionOpcode;
    private final short evtype;
    private final int deviceId;
    private final int touchId;
    private final double[] valuators;
    private final int valuatorMask;

    public XIRawTouchNotify(int deviceId, byte extensionOpcode, short evtype,
                            int touchId, double[] valuators, int valuatorMask) {
        super(GENERIC_EVENT_CODE);
        this.deviceId = deviceId;
        this.extensionOpcode = extensionOpcode;
        this.evtype = evtype;
        this.touchId = touchId;
        this.valuators = valuators;
        this.valuatorMask = valuatorMask;
    }

    @Override
    public void send(short sequenceNumber, XOutputStream outputStream) throws IOException {
        try (XStreamLock lock = outputStream.lock()) {
            // Mask length in 4-byte units. One unit holds our X/Y bits (0x03).
            short maskLenUnits = 1;

            int numAxes = valuators.length;
            int payloadBytes =
                4 +                    // mask
                (numAxes * 8) +        // axisvalues (FP3232)
                (numAxes * 8);         // axisvalues_raw (FP3232)

            int payloadLengthUnits = payloadBytes / 4;

            // Standard generic event header + xXIRawEvent fixed part (32 bytes)
            outputStream.writeByte(this.code);               // [0] type = 35 (GenericEvent)
            outputStream.writeByte(extensionOpcode);         // [1] extension opcode
            outputStream.writeShort(sequenceNumber);         // [2-3] sequence number
            outputStream.writeInt(payloadLengthUnits);       // [4-7] length of extra payload
            outputStream.writeShort(evtype);                 // [8-9] evtype = 22/23/24
            outputStream.writeShort((short) deviceId);       // [10-11] deviceid
            outputStream.writeInt((int) System.currentTimeMillis()); // [12-15] time
            outputStream.writeInt(touchId);                  // [16-19] detail = touch ID
            outputStream.writeShort((short) deviceId);       // [20-21] sourceid
            outputStream.writeShort(maskLenUnits);           // [22-23] valuators_len
            outputStream.writeInt(0);                        // [24-27] flags
            outputStream.writePad(4);                        // [28-31] pad2

            // Payload: valuator mask (4 bytes)
            outputStream.writeInt(valuatorMask);

            // Payload: axisvalues
            for (double v : valuators) {
                outputStream.writeFP3232(v);
            }

            // Payload: axisvalues_raw
            for (double v : valuators) {
                outputStream.writeFP3232(v);
            }
        }
    }
}
