#ifndef REMOTE_H
#define REMOTE_H

#include <stdint.h>
#include <pthread.h>
#include <map>
#include <vector>
#include <string>
#include <deque>
#include "Ref.h"
#include "Stack.h"
#include "Types.h"
#include "Log.h"

namespace rop {

template <typename T>
struct Skeleton {};

template <typename T>
struct Stub {};

struct Transport;
struct Port;
struct Registry;
struct Interface;
struct Remote;
struct SkeletonBase;
struct Request;
struct Return;
typedef base::Ref<Interface> InterfaceRef;
typedef base::Ref<Remote> RemoteRef;

/**
 * Super class of every skeletons.
 * Each instance corresponds to a raw object. 
 * This is destroyed when notified by notifyRemoteDestroy().
 */
struct SkeletonBase: base::RefCounted<SkeletonBase> {
    int id; // positive.
    InterfaceRef object;
    SkeletonBase (Interface *o): object(o) {}
    virtual ~SkeletonBase () {}
    virtual Request *createRequest (int methodIndex) = 0;
};

/**
 * Container of whole objects which are related to remote peer.
 * This holds skeletons and remotes.
 */
struct Registry {
    int nextSkeletonId;
    std::map<std::string,InterfaceRef> exportables; // root objects exported to peer.
    std::map<int,Remote*> remotes; // remote handles shared by stub objects
    std::map<int,SkeletonBase*> skeletons; // skeletons locating actual local objects.
    std::map<Interface*,SkeletonBase*> skeletonByExportable; // local obj -> skel

    Transport *transport; // connection to peer.

    Registry (): nextSkeletonId(1) {
        SkeletonBase *skel = createSkeleton();
        skel->id = 0;
        skeletons[0] = skel;
    }

    ~Registry ();

    void setTransport (Transport *trans) {
        transport = trans;
    }

    /**
     * Create skeleton of this registry.
     * serves getRemote(id), notifyRemoteDestroy(id)
     */
    SkeletonBase *createSkeleton ();

    void registerExportable (std::string objname, InterfaceRef exp) {
        exportables[objname] = exp;
    }

    /**
     * Returns remote object addressed by id.
     * id value should be negative. (positive value corresponds to skeleton)
     * if the remote instance is missing, this creates the one.
     */
    Remote *getRemote (int id);

    /**
     * Get a remote object exported by the peer.
     * behaves as if this was a stub.
     * NOTE that Remote instance if base::Ref-counted. 
     */
    Remote *getRemote (std::string objname);

    /**
     * Notifies to the remote peer that given remote object was no longer
     * used by local.
     */
    void notifyRemoteDestroy (int id);

    /**
     * Returns skeleton object identified by id.
     * id value should be positive. (negative value corresponds to remote)
     * if the skeleton is missing, this returns null.
     */
    SkeletonBase *getSkeleton (int id);

    /**
     * Returns skeleton object corresponding to the given raw object.
     * If the skeleton is missing, this create the one.
     */
    SkeletonBase *getSkeleton (Interface *obj);
};

/**
 * Abstraction of rpc io & thread model.
 * Subclass of this should implement missing methods which are
 * platform-dependent & policy-dependent.
 */
struct Transport {
    Registry *registry;
    std::map<int,Port*> ports;
    std::vector<Port*> freePorts;
    pthread_mutex_t monitor;
    Transport (Registry &r): registry(&r) {
        registry->setTransport(const_cast<Transport*>(this));
        pthread_mutex_init(&monitor, 0);
    }
    virtual ~Transport ();

    /** Returns a port and activate it. */
    Port* getPort (int pid);

    /** Returns a default port bound for current thread. */
    Port* getPort ();

    /** Flush out-going messages synchronously. Called by port. */
    virtual void flushPort (Port *p) = 0;

    /** Wait until port is updated. */
    virtual void waitPort (Port *p) = 0;

    /**
     * Notifies that new request was added in the given port but no thread
     * was scheduled to run it.
     * Transport is responsible for executing the requests eventually.
     */
    virtual void notifyUnhandledRequest (Port *p) = 0;
};

/**
 * Virtual duplex channel which represents each queue of incoming/outgoing
 * messages.
 */
struct Port {
    Transport *transport;
    /**
     * Port number. positive value represents that the port was initiated by
     * local thread. negative value means that the port was initiated by 
     * the remote thread.
     */
    int32_t id;
    /** waiting returns for remote call. not owned. */
    std::deque<Return*> returns;
    /** Pending call request. owned. */
    std::deque<Request*> requests;
    /**
     * Last request which was just completed.
     * Also used for call chaining.
     */
    Request *lastRequest;

    base::Stack reader;
    base::Stack writer;

    // ready to serve incoming reenterant request
    // until all return value is received.
    bool isWaiting;
    pthread_cond_t wakeCondition;

    Port (Transport *trans): transport(trans), isWaiting(false),
            lastRequest(0) {
        pthread_cond_init(&wakeCondition, 0);
    }

    virtual ~Port ();

    /*
     * read incoming byte streams from the buffer.
     * Returns either STOPPED or COMPLETE or ABORT.
     */
    base::Frame::STATE read (Buffer *buf);

    /**
     * write out-going data to the buffer.
     * Returns either STOPPED or COMPLETE or ABORT.
     */
    base::Frame::STATE write (Buffer *buf);

    /** Flush out-going messages synchronously. */
    void flush ();

    /**
     * Enqueue the return object which will hold response in order.
     * On return value arrival, queued return object will be automatically
     * removed. You can wait on the return.
     */
    void addReturn (Return *ret);

    /**
     * Blocks until all return values are vailable.
     * If there're a thread waiting on sendAndWait(), it will run the request.
     * (i.e. reenterant call)
     */
    void flushAndWait ();

    /**
     * Handle a request added into this port and shift internal request queue.
     * Returns true if a request was handled.
     */
    bool handleRequest ();
};

/**
 * Abstrctation of incoming request which holds arguments and return value.
 * Managed by port.
 */
struct Request {
    int8_t messageHead;
    base::Frame *argumentsReader;
    base::Frame *returnWriter;
    virtual ~Request () {}
    virtual void call () {}
};

/**
 * Unique handle of each skeleton of remote peer.
 * This is base::Ref-counted by stub instances, and notifies to the peer on destroy.
 */
struct Remote: base::RefCounted<Remote> {
    int id; // negative
    Registry *registry;
    ~Remote () {
        registry->notifyRemoteDestroy(id);
    }
};

/**
 * Abstraction of every rpc interfaces this framework support.
 * The real instance could be either stub or raw object. 
 * Skeleton must have null remote.
 * Stub must return null on createSkeleton().
 */
struct Interface: base::RefCounted<Interface> {
    RemoteRef remote;
    virtual SkeletonBase *createSkeleton () { return 0; }
    virtual ~Interface () {}
};

/**
 * Utility template which provides automatic skeleton creation. (optional)
 * Raw class may extend this template instead of rpc interface directly.
 * T must be the rpc interface.
 */
template <typename T>
struct Exportable: T {
    SkeletonBase *createSkeleton () { return new Skeleton<T>(this); }
};

/**
 * Return value holder.
 */
struct Return: base::Frame {
    int index; /** type of return. 0 for normal type, 1+ for exceptions */
    void *value;
    Return (): index(-1) {}
};

/**
 * Return value holder which reads the value from stream.
 */
template <const int S>
struct ReturnReader: Return {
    void *values[S];
    base::Frame *frames[S];

    STATE run (base::Stack *stack) {
        BEGIN_STEP();
        TRY_READ(int32_t, index, stack);

        NEXT_STEP();
        stack->push(frames[index]);
        value = values[index];
        CALL();

        END_STEP();
    }
};

/**
 * Return value holder which sends the value to stream.
 */
template <const int S>
struct ReturnWriter: Return {
    base::Frame *frames[S];
    int8_t messageHeader;

    ReturnWriter (): messageHeader(0x3<<6) {}

    STATE run (base::Stack *stack) {
        BEGIN_STEP();
        TRY_WRITE(int8_t, messageHeader, stack);

        NEXT_STEP();
        TRY_WRITE(int32_t, index, stack);

        NEXT_STEP();
        stack->push(frames[index]);
        CALL();

        END_STEP();
    }
};

/**
 * Object reader.
 * T must be rpc interface.
 * Read object could be either stub or raw object.
 */
template <typename T>
struct InterfaceReader: base::Frame {
    base::Ref<T> &object;
    Registry *registry;
    InterfaceReader (base::Ref<T> &o, Registry *r): object(o), registry(r) {}
    STATE run (base::Stack *stack) {
        int8_t i;
        int id;
        BEGIN_STEP();
        TRY_READ(int8_t, i, stack);
        if (!i) {
            object = 0;
            return COMPLETE;
        }

        NEXT_STEP();
        TRY_READ(int32_t, id, stack);
        id = -id; // reverse local <-> remote on receive

        if (id > 0) {
            object = reinterpret_cast<T*>(
                    registry->getSkeleton(id)->object.get());
        } else {
            object = new Stub<T>();
            object->remote = registry->getRemote(id);
        }

        END_STEP();
    }
};

struct InterfaceWriter: base::Frame {

    InterfaceRef &object;
    Registry *registry;
    int id;

    InterfaceWriter (base::Ref<Interface> &o, Registry *r): object(o), registry(r) {}

    STATE run (base::Stack *stack) {
        int8_t i;

        BEGIN_STEP();
        if (object) {
            i = 0;
            TRY_WRITE(int8_t, i, stack);
            return COMPLETE;
        } else {
            i = 1;
            TRY_WRITE(int8_t, i, stack);
        }

        NEXT_STEP();
        if (object->remote) {
            id = object->remote->id;
        } else {
            id = registry->getSkeleton(object.get())->id;
        }
        TRY_WRITE(int32_t, id, stack);

        END_STEP();
    }
};

template <const int S>
struct RequestWriter: SequenceWriter<S+3> {
    int8_t messageHead;
    int32_t objectId;
    int16_t methodIndex;
    Writer<int8_t> frame0;
    Writer<int32_t> frame1;
    Writer<int16_t> frame2;
    base::Frame **args;

    RequestWriter (int8_t h, int32_t o, int16_t m):
            messageHead(h), objectId(o), methodIndex(m),
            frame0(messageHead), frame1(objectId), frame2(methodIndex) {
        this->frames[0] = &frame0;
        this->frames[1] = &frame1;
        this->frames[2] = &frame2;
        args = this->frames+3;
    }
};

}
#endif
