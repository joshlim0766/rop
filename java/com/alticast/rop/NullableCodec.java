package com.alticast.rop;
import com.alticast.io.*;

public class NullableCodec implements Codec {

    private Codec codec;

    public NullableCodec (Codec c) {
        codec = c;
    }

    public void scan (Buffer buf) {
        if (buf.readI8() == 0) return;
        codec.scan(buf);
    }

    public Object read (Buffer buf) {
        if (buf.readI8() == 0) return null;
        return codec.read(buf);
    }

    public void write (Object obj, Buffer buf) {
        if (obj == null) {
            buf.writeI8(0);
            return;
        }

        buf.writeI8(1);
        codec.write(obj, buf);
    }
}
