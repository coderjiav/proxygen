/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <boost/heap/d_ary_heap.hpp>
#include <climits>
#include <folly/SocketAddress.h>
#include <folly/wangle/acceptor/TransportInfo.h>
#include <ostream>
#include <proxygen/lib/http/HTTPConstants.h>
#include <proxygen/lib/http/HTTPHeaderSize.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/lib/http/ProxygenErrorEnum.h>
#include <proxygen/lib/http/Window.h>
#include <proxygen/lib/http/codec/HTTPCodec.h>
#include <proxygen/lib/http/session/HTTPEvent.h>
#include <proxygen/lib/http/session/HTTPTransactionEgressSM.h>
#include <proxygen/lib/http/session/HTTPTransactionIngressSM.h>
#include <proxygen/lib/utils/AsyncTimeoutSet.h>
#include <set>

namespace proxygen {

/**
 * An HTTPTransaction represents a single request/response pair
 * for some HTTP-like protocol.  It works with a Transport that
 * performs the network processing and wire-protocol formatting
 * and a Handler that implements some sort of application logic.
 *
 * The typical sequence of events for a simple application is:
 *
 *   * The application accepts a connection and creates a Transport.
 *   * The Transport reads from the connection, parses whatever
 *     protocol the client is speaking, and creates a Transaction
 *     to represent the first request.
 *   * Once the Transport has received the full request headers,
 *     it creates a Handler, plugs the handler into the Transaction,
 *     and calls the Transaction's onIngressHeadersComplete() method.
 *   * The Transaction calls the Handler's onHeadersComplete() method
 *     and the Handler begins processing the request.
 *   * If there is a request body, the Transport streams it through
 *     the Transaction to the Handler.
 *   * When the Handler is ready to produce a response, it streams
 *     the response through the Transaction to the Transport.
 *   * When the Transaction has seen the end of both the request
 *     and the response, it detaches itself from the Handler and
 *     Transport and deletes itself.
 *   * The Handler deletes itself at some point after the Transaction
 *     has detached from it.
 *   * The Transport may, depending on the protocol, process other
 *     requests after -- or even in parallel with -- that first
 *     request.  Each request gets its own Transaction and Handler.
 *
 * For some applications, like proxying, a Handler implementation
 * may obtain one or more upstream connections, each represented
 * by another Transport, and create outgoing requests on the upstream
 * connection(s), with each request represented as a new Transaction.
 *
 * With a multiplexing protocol like SPDY on both sides of a proxy,
 * the cardinality relationship can be:
 *
 *                 +-----------+     +-----------+     +-------+
 *   (Client-side) | Transport |1---*|Transaction|1---1|Handler|
 *                 +-----------+     +-----------+     +-------+
 *                                                         1
 *                                                         |
 *                                                         |
 *                                                         1
 *                                   +---------+     +-----------+
 *                (Server-side)      |Transport|1---*|Transaction|
 *                                   +---------+     +-----------+
 *
 * A key design goal of HTTPTransaction is to serve as a protocol-
 * independent abstraction that insulates Handlers from the semantics
 * different of HTTP-like protocols.
 */

class HTTPSessionStats;
class HTTPTransaction;
class HTTPTransactionHandler {
 public:

  /**
   * Called once per transaction. This notifies the handler of which
   * transaction it should talk to and will receive callbacks from.
   */
  virtual void setTransaction(HTTPTransaction* txn) noexcept = 0;

  /**
   * Called once after a transaction successfully completes. It
   * will be called even if a read or write error happened earlier.
   * This is a terminal callback, which means that the HTTPTransaction
   * object that gives this call will be invalid after this function
   * completes.
   */
  virtual void detachTransaction() noexcept = 0;

  /**
   * Called at most once per transaction. This is usually the first
   * ingress callback. It is possible to get a read error before this
   * however. If you had previously called pauseIngress(), this callback
   * will be delayed until you call resumeIngress().
   */
  virtual void onHeadersComplete(std::unique_ptr<HTTPMessage> msg) noexcept = 0;

  /**
   * Can be called multiple times per transaction. If you had previously
   * called pauseIngress(), this callback will be delayed until you call
   * resumeIngress().
   */
  virtual void onBody(std::unique_ptr<folly::IOBuf> chain) noexcept = 0;

  /**
   * Can be called multiple times per transaction. If you had previously
   * called pauseIngress(), this callback will be delayed until you call
   * resumeIngress(). This signifies the beginning of a chunk of length
   * 'length'. You will receive onBody() after this. Also, the length will
   * be greater than zero.
   */
  virtual void onChunkHeader(size_t length) noexcept {};

  /**
   * Can be called multiple times per transaction. If you had previously
   * called pauseIngress(), this callback will be delayed until you call
   * resumeIngress(). This signifies the end of a chunk.
   */
  virtual void onChunkComplete() noexcept {};

  /**
   * Can be called any number of times per transaction. If you had
   * previously called pauseIngress(), this callback will be delayed until
   * you call resumeIngress(). Trailers can be received once right before
   * the EOM of a chunked HTTP/1.1 reponse or multiple times per
   * transaction from SPDY and HTTP/2.0 HEADERS frames.
   */
  virtual void onTrailers(std::unique_ptr<HTTPHeaders> trailers) noexcept
    = 0;

  /**
   * Can be called once per transaction. If you had previously called
   * pauseIngress(), this callback will be delayed until you call
   * resumeIngress(). After this callback is received, there will be no
   * more normal ingress callbacks received (onEgress*() and onError()
   * may still be invoked). The Handler should consider
   * ingress complete after receiving this message. This Transaction is
   * still valid, and work may still occur on it until detachTransaction
   * is called.
   */
  virtual void onEOM() noexcept = 0;

  /**
   * Can be called once per transaction. If you had previously called
   * pauseIngress(), this callback will be delayed until you call
   * resumeIngress(). After this callback is invoked, further data
   * will be forwarded using the onBody() callback. Once the data transfer
   * is completed (EOF recevied in case of CONNECT), onEOM() callback will
   * be invoked.
   */
  virtual void onUpgrade(UpgradeProtocol protocol) noexcept = 0;

  /**
   * Can be called at any time before detachTransaction(). This callback
   * implies that an error has occurred. To determine if ingress or egress
   * is affected, check the direciont on the HTTPException. If the
   * direction is INGRESS, it MAY still be possible to send egress.
   */
  virtual void onError(const HTTPException& error) noexcept = 0;

  /**
   * If the remote side's receive buffer fills up, this callback will be
   * invoked so you can attempt to stop sending to the remote side.
   */
  virtual void onEgressPaused() noexcept = 0;

  /**
   * This callback lets you know that the remote side has resumed reading
   * and you can now continue to send data.
   */
  virtual void onEgressResumed() noexcept = 0;

  /**
   * Ask the handler to construct a handler for a pushed transaction associated
   * with its transaction.
   *
   * TODO: Reconsider default implementation here. If the handler
   * does not implement, better set max initiated to 0 in a settings frame?
   */
  virtual void onPushedTransaction(HTTPTransaction* txn) noexcept {}

  virtual ~HTTPTransactionHandler() {}
};

class HTTPPushTransactionHandler : public HTTPTransactionHandler {
 public:
  virtual ~HTTPPushTransactionHandler() {}

  void onHeadersComplete(std::unique_ptr<HTTPMessage> msg) noexcept final {
    LOG(FATAL) << "push txn received headers";
  }

  void onBody(std::unique_ptr<folly::IOBuf> chain) noexcept final {
    LOG(FATAL) << "push txn received body";
  }

  void onChunkHeader(size_t length) noexcept final {
    LOG(FATAL) << "push txn received chunk header";
  }

  void onChunkComplete() noexcept final {
    LOG(FATAL) << "push txn received chunk complete";
  }

  void onTrailers(std::unique_ptr<HTTPHeaders> trailers)
    noexcept final {
    LOG(FATAL) << "push txn received trailers";
  }

  void onEOM() noexcept final {
    LOG(FATAL) << "push txn received EOM";
  }

  void onUpgrade(UpgradeProtocol protocol) noexcept final {
    LOG(FATAL) << "push txn received upgrade";
  }

  void onPushedTransaction(HTTPTransaction* txn) noexcept final {
    LOG(FATAL) << "push txn received push txn";
  }
};

class HTTPTransaction :
      public AsyncTimeoutSet::Callback {
 public:
  typedef HTTPTransactionHandler Handler;
  typedef HTTPPushTransactionHandler PushHandler;

  class Transport {
   public:
    virtual ~Transport() {}

    virtual void pauseIngress(HTTPTransaction* txn) noexcept = 0;

    virtual void resumeIngress(HTTPTransaction* txn) noexcept = 0;

    virtual void transactionTimeout(HTTPTransaction* txn) noexcept = 0;

    virtual void sendHeaders(HTTPTransaction* txn,
                             const HTTPMessage& headers,
                             HTTPHeaderSize* size) noexcept = 0;

    virtual size_t sendBody(HTTPTransaction* txn,
                            std::unique_ptr<folly::IOBuf>,
                            bool eom) noexcept = 0;

    virtual size_t sendChunkHeader(HTTPTransaction* txn,
                                   size_t length) noexcept = 0;

    virtual size_t sendChunkTerminator(HTTPTransaction* txn) noexcept = 0;

    virtual size_t sendTrailers(HTTPTransaction* txn,
                                const HTTPHeaders& trailers) noexcept = 0;

    virtual size_t sendEOM(HTTPTransaction* txn) noexcept = 0;

    virtual size_t sendAbort(HTTPTransaction* txn,
                             ErrorCode statusCode) noexcept = 0;

    virtual size_t sendWindowUpdate(HTTPTransaction* txn,
                                    uint32_t bytes) noexcept = 0;

    virtual void notifyPendingEgress() noexcept = 0;

    virtual void detach(HTTPTransaction* txn) noexcept = 0;

    virtual void notifyIngressBodyProcessed(uint32_t bytes) noexcept = 0;

    virtual const folly::SocketAddress& getLocalAddress()
      const noexcept = 0;

    virtual const folly::SocketAddress& getPeerAddress()
      const noexcept = 0;

    virtual void describe(std::ostream&) const = 0;

    virtual const folly::TransportInfo& getSetupTransportInfo() const noexcept = 0;

    virtual bool getCurrentTransportInfo(folly::TransportInfo* tinfo) = 0;

    virtual const HTTPCodec& getCodec() const noexcept = 0;

    virtual bool isDraining() const = 0;

    virtual HTTPTransaction* newPushedTransaction(
      HTTPCodec::StreamID assocStreamId,
      HTTPTransaction::PushHandler* handler,
      int8_t priority) noexcept = 0;
  };

  /**
   * Callback interface to be notified of events on the byte stream.
   */
  class TransportCallback {
   public:
    virtual void firstHeaderByteFlushed() noexcept = 0;

    virtual void firstByteFlushed() noexcept = 0;

    virtual void lastByteFlushed() noexcept = 0;

    virtual void lastByteAcked(std::chrono::milliseconds latency) noexcept = 0;

    virtual void headerBytesGenerated(HTTPHeaderSize& size) noexcept = 0;

    virtual void headerBytesReceived(const HTTPHeaderSize& size) noexcept = 0;

    virtual void bodyBytesGenerated(size_t nbytes) noexcept = 0;

    virtual void bodyBytesReceived(size_t size) noexcept = 0;

    virtual ~TransportCallback() {};
  };

  struct LessP {
    bool operator()(const HTTPTransaction* left,
                    const HTTPTransaction* right) const {
      // larger values are logically smaller
      return left->priority_ > right->priority_;
    }
  };

  typedef boost::heap::d_ary_heap<HTTPTransaction*,
                                  boost::heap::arity<2>,
                                  boost::heap::stable<false>,
                                  boost::heap::compare<LessP>,
                                  boost::heap::mutable_<true>,
                                  boost::heap::constant_time_size<true>
                                  > PriorityQueue;

  /**
   * readBufLimit and sendWindow are only used if useFlowControl is
   * true. Furthermore, if flow control is enabled, no guarantees can be
   * made on the borders of the L7 chunking/data frames of the outbound
   * messages.
   *
   * priority is only used by SPDY. The -1 default makes sure that all
   * plain HTTP transactions land up in the same queue as the control data.
   */
  HTTPTransaction(TransportDirection direction,
                  HTTPCodec::StreamID id,
                  uint32_t seqNo,
                  Transport& transport,
                  PriorityQueue& egressQueue,
                  AsyncTimeoutSet* transactionTimeouts,
                  HTTPSessionStats* stats = nullptr,
                  bool useFlowControl = false,
                  uint32_t receiveInitialWindowSize = 0,
                  uint32_t sendInitialWindowSize = 0,
                  int8_t priority = -1,
                  HTTPCodec::StreamID assocStreamId = 0);

  virtual ~HTTPTransaction() override;

  HTTPCodec::StreamID getID() const { return id_; }

  uint32_t getSequenceNumber() const { return seqNo_; }

  const Transport& getTransport() const { return transport_; }

  Transport& getTransport() { return transport_; }

  void setHandler(Handler* handler) {
    handler_ = handler;
    if (handler_) {
      handler_->setTransaction(this);
    }
  }

  const Handler* getHandler() const {
    return handler_;
  }

  uint32_t getPriority() const {
    return priority_;
  }

  HTTPTransactionEgressSM::State getEgressState() const {
    return egressState_;
  }

  HTTPTransactionIngressSM::State getIngressState() const {
    return ingressState_;
  }

  bool isUpstream() const {
    return direction_ == TransportDirection::UPSTREAM;
  }

  bool isDownstream() const {
    return direction_ == TransportDirection::DOWNSTREAM;
  }

  void getLocalAddress(folly::SocketAddress& addr) const {
    addr = transport_.getLocalAddress();
  }

  void getPeerAddress(folly::SocketAddress& addr) const {
    addr = transport_.getPeerAddress();
  }

  const folly::SocketAddress& getLocalAddress()
    const noexcept {
    return transport_.getLocalAddress();
  }

  const folly::SocketAddress& getPeerAddress()
    const noexcept {
    return transport_.getPeerAddress();
  }

  const folly::TransportInfo& getSetupTransportInfo() const noexcept {
    return transport_.getSetupTransportInfo();
  }

  void getCurrentTransportInfo(folly::TransportInfo* tinfo) const {
    transport_.getCurrentTransportInfo(tinfo);
  }

  HTTPSessionStats* getSessionStats() const {
    return stats_;
  }

  /**
   * Check whether more response is expected. One or more 1xx status
   * responses can be received prior to the regular response.
   * Note: 101 is handled by the codec using a separate onUpgrade callback
   */
  virtual bool extraResponseExpected() const {
    return (lastResponseStatus_ >= 100 && lastResponseStatus_ < 200)
        && lastResponseStatus_ != 101;
  }

  /**
   * Change the size of the receive window and propagate the change to the
   * remote end using a window update.
   *
   * TODO: when HTTPSession sends a SETTINGS frame indicating a
   * different initial window, it should call this function on all its
   * transactions.
   */
  virtual void setReceiveWindow(uint32_t capacity);

  /**
   * Get the receive window of the transaction
   */
  virtual const Window& getReceiveWindow() const {
    return recvWindow_;
  }

  uint32_t getMaxDeferredSize() {
    return maxDeferredIngress_;
  }

  /**
   * Invoked by the session when the ingress headers are complete
   */
  void onIngressHeadersComplete(std::unique_ptr<HTTPMessage> msg);

  /**
   * Invoked by the session when some or all of the ingress entity-body has
   * been parsed.
   */
  void onIngressBody(std::unique_ptr<folly::IOBuf> chain);

  /**
   * Invoked by the session when a chunk header has been parsed.
   */
  void onIngressChunkHeader(size_t length);

  /**
   * Invoked by the session when the CRLF terminating a chunk has been parsed.
   */
  void onIngressChunkComplete();

  /**
   * Invoked by the session when the ingress trailers have been parsed.
   */
  void onIngressTrailers(std::unique_ptr<HTTPHeaders> trailers);

  /**
   * Invoked by the session when the session and transaction need to be
   * upgraded to a different protocol
   */
  void onIngressUpgrade(UpgradeProtocol protocol);

  /**
   * Invoked by the session when the ingress message is complete.
   */
  void onIngressEOM();

  /**
   * Invoked by the session when there is an error (e.g., invalid syntax,
   * TCP RST) in either the ingress or egress stream. Note that this
   * message is processed immediately even if this transaction normally
   * would queue ingress.
   *
   * @param error Details for the error. This exception also has
   * information about whether the error applies to the ingress, egress,
   * or both directions of the transaction
   */
  void onError(const HTTPException& error);

  /**
   * Invoked by the session when there is a timeout on the ingress stream.
   * Note that each transaction has its own timer but the session
   * is the effective target of the timer.
   */
  void onIngressTimeout();

  /**
   * Invoked by the session when the remote endpoint of this transaction
   * signals that it has consumed 'amount' bytes. This is only for
   * versions of HTTP that support per transaction flow control.
   */
  void onIngressWindowUpdate(uint32_t amount);

  /**
   * Invoked by the session when the remote endpoint signals that we
   * should change our send window. This is only for
   * versions of HTTP that support per transaction flow control.
   */
  void onIngressSetSendWindow(uint32_t newWindowSize);

  /**
   * Notify this transaction that it is ok to egress.  Returns true if there
   * is additional pending egress
   */
  bool onWriteReady(uint32_t maxEgress);

  /**
   * Invoked by the session when there is a timeout on the egress stream.
   */
  void onEgressTimeout();

  /**
   * Invoked by the session when the first header byte is flushed.
   */
  void onEgressHeaderFirstByte();

  /**
   * Invoked by the session when the first byte is flushed.
   */
  void onEgressBodyFirstByte();

  /**
   * Invoked by the session when the first byte is flushed.
   */
  void onEgressBodyLastByte();

  /**
   * Invoked when the ACK_LATENCY event is delivered
   *
   * @param latency the time between the moment when the last byte was sent
   *        and the moment when we received the ACK from the client
   */
  void onEgressLastByteAck(std::chrono::milliseconds latency);

  /**
   * Invoked by the handlers that are interested in tracking
   * performance stats.
   */
  void setTransportCallback(TransportCallback* cb) {
    transportCallback_ = cb;
  }

  /**
   * @return true if egress has started on this transaction.
   */
  bool isIngressStarted() const {
    return ingressState_ != HTTPTransactionIngressSM::State::Start;
  }

  /**
   * @return true iff the ingress EOM has been queued in HTTPTransaction
   * but the handler has not yet been notified of this event.
   */
  bool isIngressEOMQueued() const {
    return ingressState_ == HTTPTransactionIngressSM::State::EOMQueued;
  }

  /**
   * @return true iff the handler has been notified of the ingress EOM.
   */
  bool isIngressComplete() const {
    return ingressState_ == HTTPTransactionIngressSM::State::ReceivingDone;
  }

  /**
   * @return true iff onIngressEOM() has been called.
   */
  bool isIngressEOMSeen() const {
    return isIngressEOMQueued() || isIngressComplete();
  }

  /**
   * @return true if egress has started on this transaction.
   */
  bool isEgressStarted() const {
    return egressState_ != HTTPTransactionEgressSM::State::Start;
  }

  /**
   * @return true iff sendEOM() has been called, but the eom has not been
   * flushed to the socket yet.
   */
  bool isEgressEOMQueued() const {
    return egressState_ == HTTPTransactionEgressSM::State::EOMQueued;
  }

  /**
   * @return true iff the egress EOM has been flushed to the socket.
   */
  bool isEgressComplete() const {
    return egressState_ == HTTPTransactionEgressSM::State::SendingDone;
  }

  /**
   * @return true iff sendEOM() has been called.
   */
  bool isEgressEOMSeen() const {
    return isEgressEOMQueued() || isEgressComplete();
  }

  /**
   * @return true if we can send headers on this transaction
   */
  bool canSendHeaders() const {
    return HTTPTransactionEgressSM::canTransit(
        egressState_,
        HTTPTransactionEgressSM::Event::sendHeaders)
      && !isEgressComplete();
  }

  /**
   * Send the egress message headers to the Transport. This method does
   * not actually write the message out on the wire immediately. All
   * writes happen at the end of the event loop at the earliest.
   * Note: This method should be called once per message unless the first
   * headers sent indicate a 1xx status.
   *
   * @param headers  Message headers
   */
  virtual void sendHeaders(const HTTPMessage& headers);

  /**
   * Send part or all of the egress message body to the Transport. If flow
   * control is enabled, the chunk boundaries may not be respected.
   * This method does not actually write the message out on the wire
   * immediately. All writes happen at the end of the event loop at the
   * earliest.
   * Note: This method may be called zero or more times per message.
   *
   * @param body Message body data; the Transport will take care of
   *             applying any necessary protocol framing, such as
   *             chunk headers.
   */
  virtual void sendBody(std::unique_ptr<folly::IOBuf> body);

  /**
   * Write any protocol framing required for the subsequent call(s)
   * to sendBody(). This method does not actually write the message out on
   * the wire immediately. All writes happen at the end of the event loop
   * at the earliest.
   * @param length  Length in bytes of the body data to follow.
   */
  virtual void sendChunkHeader(size_t length) {
    CHECK(HTTPTransactionEgressSM::transit(
            egressState_, HTTPTransactionEgressSM::Event::sendChunkHeader));
    // TODO: move this logic down to session/codec
    if (!transport_.getCodec().supportsParallelRequests()) {
      chunkHeaders_.emplace_back(Chunk(length));
    }
  }

  /**
   * Write any protocol syntax needed to terminate the data. This method
   * does not actually write the message out on the wire immediately. All
   * writes happen at the end of the event loop at the earliest.
   * Frame begun by the last call to sendChunkHeader().
   */
  virtual void sendChunkTerminator() {
    CHECK(HTTPTransactionEgressSM::transit(
            egressState_, HTTPTransactionEgressSM::Event::sendChunkTerminator));
  }

  /**
   * Send message trailers to the Transport. This method does
   * not actually write the message out on the wire immediately. All
   * writes happen at the end of the event loop at the earliest.
   * Note: This method may be called at most once per message.
   *
   * @param trailers  Message trailers.
   */
  virtual void sendTrailers(const HTTPHeaders& trailers) {
    CHECK(HTTPTransactionEgressSM::transit(
            egressState_, HTTPTransactionEgressSM::Event::sendTrailers));
    if (transport_.getCodec().supportsParallelRequests()) {
      // SPDY supports trailers whenever
      size_t nbytes = transport_.sendTrailers(this, trailers);
      if (transportCallback_) {
        HTTPHeaderSize size;
        size.uncompressed = nbytes;
        transportCallback_->headerBytesGenerated(size);
      }
    } else {
      // HTTP requires them to go right before EOM
      trailers_.reset(new HTTPHeaders(trailers));
    }
  }

  /**
   * Finalize the egress message; depending on the protocol used
   * by the Transport, this may involve sending an explicit "end
   * of message" indicator. This method does not actually write the
   * message out on the wire immediately. All writes happen at the end
   * of the event loop at the earliest.
   *
   * If the ingress message also is complete, the transaction may
   * detach itself from the Handler and Transport and delete itself
   * as part of this method.
   *
   * Note: Either this method or sendAbort() should be called once
   *       per message.
   */
  virtual void sendEOM();

  /**
   * Terminate the transaction. Depending on the underlying protocol, this
   * may cause the connection to close or write egress bytes. This method
   * does not actually write the message out on the wire immediately. All
   * writes happen at the end of the event loop at the earliest.
   *
   * This function may also cause additional callbacks such as
   * detachTransaction() to the handler either immediately or after it returns.
   */
  virtual void sendAbort();

  /**
   * Pause ingress processing.  Upon pause, the HTTPTransaction
   * will call its Transport's pauseIngress() method.  The Transport
   * should make a best effort to stop invoking the HTTPTransaction's
   * onIngress* callbacks.  If the Transport does invoke any of those
   * methods while the transaction is paused, however, the transaction
   * will queue the ingress events and data and delay delivery to the
   * Handler until the transaction is unpaused.
   */
  virtual void pauseIngress();

  /**
   * Resume ingress processing. Only useful after a call to pauseIngress().
   */
  virtual void resumeIngress();

  /**
   * @return true iff ingress processing is paused for the handler
   */
  bool isIngressPaused() const { return ingressPaused_; }

  /**
   * Pause egress generation. HTTPTransaction may call its Handler's
   * onEgressPaused() method if there is a state change as a result of
   * this call.
   *
   * On receiving onEgressPaused(), the Handler should make a best effort
   * to stop invoking the HTTPTransaction's egress generating methods.  If
   * the Handler does invoke any of those methods while the transaction is
   * paused, however, the transaction will forward them anyway, unless it
   * is a body event. If flow control is enabled, body events will be
   * buffered for later transmission when egress is unpaused.
   */
  void pauseEgress();

  /**
   * Resume egress generation. The Handler's onEgressResumed() will not be
   * invoked if the HTTP/2 send window is full or there is too much
   * buffered egress data on this transaction already. In that case,
   * once the send window is not full or the buffer usage decreases, the
   * handler will finally get onEgressResumed().
   */
  void resumeEgress();

  /**
   * @return true iff egress processing is paused for the handler
   */
  bool isEgressPaused() const { return handlerEgressPaused_; }

  /**
   * @return true iff this transaction can be used to push resources to
   * the remote side.
   */
  bool supportsPushTransactions() const {
    return direction_ == TransportDirection::DOWNSTREAM &&
      transport_.getCodec().supportsPushTransactions();
  }

  /**
   * Create a new pushed transaction associated with this transaction,
   * and assign the given handler and priority.
   *
   * @return the new transaction for the push, or nullptr if a new push
   * transaction is impossible right now.
   */
  virtual HTTPTransaction* newPushedTransaction(
    HTTPPushTransactionHandler* handler, uint8_t priority) {
    if (isEgressEOMSeen()) {
      return nullptr;
    }
    auto txn = transport_.newPushedTransaction(id_, handler, priority);
    if (txn) {
      pushedTransactions_.insert(txn->getID());
    }
    return txn;
  }

  /**
   * Invoked by the session (upstream only) when a new pushed transaction
   * arrives.  The txn's handler will be notified and is responsible for
   * installing a handler.  If no handler is installed in the callback,
   * the pushed transaction will be aborted.
   */
  bool onPushedTransaction(HTTPTransaction* txn);

  /**
   * True if this transaction is a server push transaction
   */
  bool isPushed() const {
    return assocStreamId_ != 0;
  }

  /**
   * Returns the associated transaction ID for pushed transactions, 0 otherwise
   */
  HTTPCodec::StreamID getAssocTxnId() const {
    return assocStreamId_;
  }

  /**
   * Get a set of server-pushed transactions associated with this transaction.
   */
  const std::set<HTTPCodec::StreamID>& getPushedTransactions() const {
    return pushedTransactions_;
  }

  /**
   * Remove the pushed txn ID from the set of pushed txns
   * associated with this txn.
   */
  void removePushedTransaction(HTTPCodec::StreamID pushStreamId) {
    pushedTransactions_.erase(pushStreamId);
  }

  /**
   * Schedule or refresh the timeout for this transaction
   */
  void refreshTimeout() {
    if (transactionTimeouts_) {
      transactionTimeouts_->scheduleTimeout(this);
    }
  }

  /**
   * Tests if the first byte has already been sent, and if it
   * hasn't yet then it marks it as sent.
   */
  bool testAndSetFirstByteSent() {
    bool ret = firstByteSent_;
    firstByteSent_ = true;
    return ret;
  }

  bool testAndClearActive() {
    bool ret = inActiveSet_;
    inActiveSet_ = false;
    return ret;
  }

  /**
   * Tests if the very first byte of Header has already been set.
   * If it hasn't yet, it marks it as sent.
   */
  bool testAndSetFirstHeaderByteSent() {
    bool ret = firstHeaderByteSent_;
    firstHeaderByteSent_ = true;
    return ret;
  }

  /**
   * Timeout callback for this transaction.  The timer is active while
   * until the ingress message is complete or terminated by error.
   */
  void timeoutExpired() noexcept {
    transport_.transactionTimeout(this);
  }

  /**
   * Write a description of the transaction to a stream
   */
  void describe(std::ostream& os) const;

  /**
   * Set the maximum egress body size for any outbound body bytes
   */
  static void setFlowControlledBodySizeLimit(uint64_t limit) {
    egressBodySizeLimit_ = limit;
  }

  /**
   * Helper class that:
   * 1. Increments callbackDepth_ to prevent destruction
   *    within a scope, and
   * 2. calls checkForCompletion() at the end of the scope.
   *
   * Every method except the constructor, destructor, and
   * checkForCompletion() should instantiate a CallbackGuard
   * as a local variable.
   */
  class CallbackGuard {
   public:
    explicit CallbackGuard(HTTPTransaction& txn):
    txn_(txn) {
      ++txn_.callbackDepth_;
    }
    CallbackGuard(CallbackGuard const & other): txn_(other.txn_) {
      ++txn_.callbackDepth_;
    }
    ~CallbackGuard() {
      if (0 == --txn_.callbackDepth_) {
        txn_.checkForCompletion();
      }
    }

    HTTPTransaction& peekTransaction() { return txn_; }
   private:
    HTTPTransaction& txn_;
  };

 private:
  HTTPTransaction(const HTTPTransaction&) = delete;
  HTTPTransaction& operator=(const HTTPTransaction&) = delete;

  /**
   * Check whether the ingress and egress messages are both complete;
   * if they are, detach from the Transport and Handler and delete this
   * HTTPTransaction.
   */
  void checkForCompletion();

  /**
   * Invokes the handler's onEgressPaused/Resumed if the handler's pause
   * state needs updating
   */
  void updateHandlerPauseState();

  bool mustQueueIngress() const;

  /**
   * Check if deferredIngress_ points to some queue before pushing HTTPEvent
   * to it.
   */
  void checkCreateDeferredIngress();

  /**
   * Implementation of sending an abort for this transaction.
   */
  void sendAbort(ErrorCode statusCode);

  // Internal implementations of the ingress-related callbacks
  // that work whether the ingress events are immediate or deferred.
  void processIngressHeadersComplete(std::unique_ptr<HTTPMessage> msg);
  void processIngressBody(std::unique_ptr<folly::IOBuf> chain, size_t len);
  void processIngressChunkHeader(size_t length);
  void processIngressChunkComplete();
  void processIngressTrailers(std::unique_ptr<HTTPHeaders> trailers);
  void processIngressUpgrade(UpgradeProtocol protocol);
  void processIngressEOM();

  void sendBodyFlowControlled(std::unique_ptr<folly::IOBuf> body = nullptr);
  size_t sendBodyNow(std::unique_ptr<folly::IOBuf> body, size_t bodyLen,
                     bool eom);
  size_t sendEOMNow();
  void onDeltaSendWindowSize(int32_t windowDelta);

  void notifyTransportPendingEgress();

  size_t sendDeferredBody(uint32_t maxEgress);

  bool isEnqueued() const { return enqueued_; }

  void dequeue() {
    DCHECK(isEnqueued());
    egressQueue_.erase(queueHandle_);
    enqueued_ = false;
  }

  bool hasPendingEOM() const {
    return deferredEgressBody_.chainLength() == 0 &&
      isEgressEOMQueued();
  }

  bool isExpectingIngress() const;

  void updateReadTimeout();

  /**
   * Causes isIngressComplete() to return true, removes any queued
   * ingress, and cancels the read timeout.
   */
  void markIngressComplete();

  /**
   * Causes isEgressComplete() to return true, removes any queued egress,
   * and cancels the write timeout.
   */
  void markEgressComplete();

  /**
   * Validates the ingress state transition. Returns false and sends an
   * abort with PROTOCOL_ERROR if the transition fails. Otherwise it
   * returns true.
   */
  bool validateIngressStateTransition(HTTPTransactionIngressSM::Event);

  /**
   * Flushes any pending window updates.  This can happen from setReceiveWindow
   * or sendHeaders depending on transaction state.
   */
  void flushWindowUpdate();

  /**
   * Queue to hold any events that we receive from the Transaction
   * while the ingress is supposed to be paused.
   */
  std::unique_ptr<std::queue<HTTPEvent>> deferredIngress_;

  uint32_t maxDeferredIngress_{0};

  /**
   * Queue to hold any body bytes to be sent out
   * while egress to the remote is supposed to be paused.
   */
  folly::IOBufQueue deferredEgressBody_{folly::IOBufQueue::cacheChainLength()};

  const TransportDirection direction_;
  HTTPCodec::StreamID id_;
  uint32_t seqNo_;
  Handler* handler_{nullptr};
  Transport& transport_;
  HTTPTransactionEgressSM::State egressState_{
    HTTPTransactionEgressSM::getNewInstance()};
  HTTPTransactionIngressSM::State ingressState_{
    HTTPTransactionIngressSM::getNewInstance()};
  AsyncTimeoutSet* transactionTimeouts_{nullptr};
  HTTPSessionStats* stats_{nullptr};

  /**
   * The recv window and associated data. This keeps track of how many
   * bytes we are allowed to buffer.
   */
  Window recvWindow_;

  /**
   * The send window and associated data. This keeps track of how many
   * bytes we are allowed to send and have outstanding.
   */
  Window sendWindow_;

  TransportCallback* transportCallback_{nullptr};

  /**
   * Number of callbacks currently active.  Used to prevent destruction
   * while in a callback that might turn around and invoke some method
   * of this object.
   */
  unsigned callbackDepth_{0};

  /**
   * Trailers to send, if any.
   */
  std::unique_ptr<HTTPHeaders> trailers_;

  struct Chunk {
    explicit Chunk(size_t inLength) : length(inLength), headerSent(false) {}
    size_t length;
    bool headerSent;
  };
  std::list<Chunk> chunkHeaders_;

  /**
   * Reference to our priority queue
   */
  PriorityQueue& egressQueue_;

  /**
   * Handle to our position in the priority queue. Only valid when
   * enqueued_ == true
   */
  PriorityQueue::handle_type queueHandle_;

  /**
   * bytes we need to acknowledge to the remote end using a window update
   */
  int32_t recvToAck_{0};

  /**
   * ID of request transaction (for pushed txns only)
   */
  HTTPCodec::StreamID assocStreamId_{0};

  /**
   * Set of all push transactions IDs associated with this transaction.
   */
  std::set<HTTPCodec::StreamID> pushedTransactions_;

  /**
   * SPDY priority in the high bits, randomness in the low bits
   */
  uint32_t priority_;

  /**
   * If this transaction represents a request (ie, it is backed by an
   * HTTPUpstreamSession) , this field indicates the last response status
   * received from the server. If this transaction represents a response,
   * this field indicates the last status we've sent. For instances, this
   * could take on multiple 1xx values, and then take on 200.
   */
  uint16_t lastResponseStatus_{0};

  bool ingressPaused_:1;
  bool egressPaused_:1;
  bool handlerEgressPaused_:1;
  bool useFlowControl_:1;
  bool aborted_:1;
  bool deleting_:1;
  bool enqueued_:1;
  bool firstByteSent_:1;
  bool firstHeaderByteSent_:1;
  bool inResume_:1;
  bool inActiveSet_:1;

  static uint64_t egressBodySizeLimit_;
  static uint64_t egressBufferLimit_;
};

/**
 * Write a description of an HTTPTransaction to an ostream
 */
std::ostream& operator<<(std::ostream& os, const HTTPTransaction& txn);

} // proxygen
