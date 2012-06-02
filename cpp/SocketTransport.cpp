#include <sys/select.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include "Remote.h"
#include "SocketTransport.h"
#include "Log.h"

using namespace std;
using namespace base;
using namespace rop;

void SocketTransport::loop ()
{
    Log l("loop ");
    fd_set infdset;
    fd_set expfdset;
    int nfds = inFd+1;

    loopThread = pthread_self();
    isLooping = true;

    while (true) {
        FD_ZERO(&infdset);
        FD_ZERO(&expfdset);
        FD_SET(inFd, &infdset);
        FD_SET(inFd, &expfdset);
        l.info("blocked for reading.\n");
        if (pselect(nfds, &infdset, 0, &expfdset, 0, 0) <= 0) {
            return;
        }
        if (FD_ISSET(inFd, &expfdset)) {
            return;
        }

        registry->lock();

        if (FD_ISSET(inFd, &infdset)) {
            l.info("got message.\n");
            tryRead();
        }

        // execute processors
        for (map<int,Port*>::iterator i = registry->ports.begin();
                i != registry->ports.end();) {
            Port *p = (i++)->second;
            if (!p->requests.empty()) {
                l.info("executing request...\n");
            }
            while (p->processRequest());
            registry->unsafeReleasePort(p);
        }

        registry->unlock();
    }
}

void SocketTransport::tryRead ()
{
    Log l("read ");

    while (true) {

        // fill in buffer
        inBuffer.moveFront();
        int r;
        r = read(inFd, inBuffer.end(), inBuffer.margin());
        if (r == 0) {
            // eof
            break;
        } if (r < 0) {
            // fail or EWOULDBLOCK
            break;
        }
        inBuffer.grow(r);
        l.debug("read %d bytes.\n", r);

begin_message:

        // on start of message frame...
        if (inPort == 0) {
            // need port id and payload length
            if (inBuffer.size() < 4) {
                break;
            }
            int p = inBuffer.read();
            p = (p << 8) + inBuffer.read();
            p = (p << 8) + inBuffer.read();
            p = (p << 8) + inBuffer.read();
            inPort = registry->unsafeGetPort(-p); // reverse local<->remote port id
            l.debug("getting port:%d\n", -p);
        }

        switch (inPort->decode(&inBuffer)) {
        case Frame::COMPLETE:
            inPort = 0;
            goto begin_message;
        case Frame::STOPPED:
            break; // continue reading
        case Frame::ABORTED:
        default: // not expected.
            // handle it? could be handled in upper layer.
            l.error("Unexpected IO exception.\n");
            return;
        }
    }
}

void SocketTransport::waitWritable ()
{
    fd_set infdset;
    fd_set outfdset;
    fd_set expfdset;
    bool isLoopThread = false;
    int nfds;

    pthread_t self = pthread_self();
    if (pthread_equal(self, loopThread)) {
        isLoopThread = true;
    }

    if (isLoopThread) {
        nfds = ((outFd > inFd) ? outFd : inFd ) + 1;
    } else {
        nfds = outFd + 1;
    }

    FD_ZERO(&infdset);
    FD_ZERO(&outfdset);
    FD_ZERO(&expfdset);
    FD_SET(outFd, &outfdset);
    FD_SET(outFd, &expfdset);
    if (isLoopThread) {
        FD_SET(inFd, &infdset);
        FD_SET(inFd, &expfdset);
    }

    registry->unlock();
    int ret = pselect(nfds, &infdset, &outfdset, &expfdset, 0, 0);
    registry->lock();
    if (ret <= 0) {
        return;
    }

    if (FD_ISSET(inFd, &expfdset) || FD_ISSET(outFd, &expfdset)) {
        return;
    }

    if (FD_ISSET(inFd, &infdset)) {
        tryRead();
    }
}

void SocketTransport::send (Port *p)
{
    Log l("flush ");
    while (isSending) {
        l.trace("waiting for lock...\n");
        pthread_cond_wait(&writable, &registry->monitor);
    }
    isSending = true;

    l.debug("sending port:%d...\n", p->id);
    outBuffer.write(p->id >> 24);
    outBuffer.write(p->id >> 16);
    outBuffer.write(p->id >> 8);
    outBuffer.write(p->id);

    while (true) {

        // fill in buffer
        if (p->writer.frame) {
            l.info("... write to buffer\n");
            if (p->encode(&outBuffer) == Frame::ABORTED) {
                // TODO: handle abortion.
                l.error("ABORTED..?\n");
            }
        } else {
            if (outBuffer.size() == 0) {
                // all flushed.
                break;
            }
        }

        int w;
        outBuffer.moveFront();
        l.debug("sending message...\n");
        w = ::write(outFd, outBuffer.begin(), outBuffer.size());
        if (w >= 0) {
            l.debug("sent %d bytes.\n", w);
            outBuffer.drop(w);
            continue;
        }

        if (errno != EWOULDBLOCK) {
            break;
        }

        l.debug("buffer full...\n");
        waitWritable();
    }

    isSending = false;
    pthread_cond_signal(&writable);
}

void SocketTransport::waitReadable ()
{
    fd_set infdset;
    fd_set expfdset;

    FD_ZERO(&infdset);
    FD_ZERO(&expfdset);
    FD_SET(inFd, &infdset);
    FD_SET(inFd, &expfdset);

    registry->unlock();
    pselect(inFd+1, &infdset, 0, &expfdset, 0, 0); // care return value?
    registry->lock();
}

void SocketTransport::receive (Port *p)
{
    Log l("waitport ");
    if (isLooping) {
        if (!pthread_equal(pthread_self(), loopThread)) {
            l.debug("waiting on condvar for port:%d\n", p->id);
            pthread_cond_wait(&p->updated, &registry->monitor);
            return;
        }
    }

    waitReadable();
    tryRead();
}
