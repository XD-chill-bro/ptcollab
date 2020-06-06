#include "SequencingServer.h"

#include <QDataStream>
#include <QMessageBox>
#include <QTcpSocket>
SequencingServer::SequencingServer(QString filename, int port, QObject *parent)
    : QObject(parent),
      m_server(new QTcpServer(this)),
      m_sessions(),
      m_file(filename, this),
      m_next_uid(0) {
  if (!m_file.open(QIODevice::ReadOnly)) {
    qFatal("File cannot be opened");
    return;
  }
  if (!m_server->listen(QHostAddress::Any, port)) {
    qFatal("Unable to start TCP server");
    return;
  }
  qInfo() << "Listening on" << m_server->serverPort();

  connect(m_server, &QTcpServer::newConnection, this,
          &SequencingServer::newClient);
}

int SequencingServer::port() { return m_server->serverPort(); }

void SequencingServer::newClient() {
  QTcpSocket *conn = m_server->nextPendingConnection();
  qInfo() << "New connection" << conn->peerAddress();

  ServerSession *session =
      new ServerSession(this, conn, m_file, m_history, m_next_uid++);
  connect(session, &ServerSession::newRemoteAction, this,
          &SequencingServer::broadcastMessage);
  m_sessions.push_back(session);
}

void SequencingServer::broadcastMessage(const RemoteActionWithUid &action) {
  // iterate over list of conns, prune inactive ones, and send action to active
  // ones
  qInfo() << "Broadcasting action" << action.uid << action.action.idx;
  int sent = 0;
  for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
    while (!(*it)->isConnected()) {
      delete *it;
      it = m_sessions.erase(it);
    }
    if (it == m_sessions.end()) break;
    (*it)->writeRemoteAction(action);
    ++sent;
  }
  qInfo() << "Sent (" << action.uid << action.action.idx << ") to" << sent
          << "clients";
}