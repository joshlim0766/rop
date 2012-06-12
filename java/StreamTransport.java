import java.util.*;
import java.io.*;

public class StreamTransport extends Transport implements Runnable {
    private IOContext inContext = new IOContext();
    private IOContext outContext = new IOContext();
    private InputStream inputStream;
    private OutputStream outputStream;
    private Object writeLock = new Object();
    private Thread readThread;

    public StreamTransport (InputStream is, OutputStream os) {
        super();
        inputStream = is;
        outputStream = os;
        inContext.registry = registry;
        inContext.buffer = new Buffer();
        outContext.registry = registry;
        outContext.buffer = new Buffer();
        readThread = new Thread(this);
        readThread.start();
    }

    public void send (Port p) {
        try {
            synchronized (writeLock) {
                outContext.buffer.writeI32(p.id);
                Cont w = p.writer.start(null, null);
                do {
                    w = outContext.runUntilBlocked(w);
                    outContext.buffer.readTo(outputStream);
                    outContext.buffer.moveFront();
                } while (w != null);
            }
        } catch (Throwable t) {
            t.printStackTrace();
            registry.dispose();
        }
    }

    // reading thread
    public void run () {
        Port inPort = null;
        Cont r = null;
        try {
            Buffer buf = inContext.buffer;
            while (true) {
                buf.moveFront();
                System.out.println("Blocked until next read");
                buf.writeFrom(inputStream);
                System.out.println("Got message in bytes:"+buf.size());
                if (r == null) {
                    if (buf.size() < 4) continue;
                    int p = buf.read();
                    p = (p<<8)+buf.read();
                    p = (p<<8)+buf.read();
                    p = (p<<8)+buf.read();
                    p = -p;
                    System.out.println("Reading from port:"+p);
                    synchronized (registry) {
                        inPort = registry.getPort(p);
                        synchronized (New.class) {
                            r = New.messageReader(inPort).start(null);
                        }
                    }
                }
                r = inContext.runUntilBlocked(r);
            }
        } catch (Throwable t) {
            t.printStackTrace();
            registry.dispose();
        }
    }

    public void dispose () {
        readThread.interrupt();
    }

    public void receive (Port p) {
        synchronized (p) {
            try {
                p.wait();
            } catch (InterruptedException e) {}
        }
    }

    public void notifyUnhandledRequest (final Port p) {
        synchronized (p) {
            if (p.processingThread != null) {
                throw new IllegalStateException("request is being processed.");
            }
            p.processingThread = new Thread() {
                public void run () {
                    while (p.processRequest());
                    synchronized (p) {
                        p.processingThread = null;
                    }
                }
            };
            p.processingThread.start();
        }
    }
}