#include "debugClient.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <memory>

void DebugClient::Request(const QString& command, Callback done) {
	if (m_busy) {
		done({}, tr("request already in flight"));
		return;
	}
	m_busy = true;

	auto*                socket   = new QTcpSocket(this);
	auto                 buffer   = std::make_shared<QByteArray>();
	auto                 finished = std::make_shared<bool>(false);
	QPointer<DebugClient> self(this);
	const auto finish = [self, socket, done, finished](const QJsonObject& reply,
	                                                   const QString&     error) {
		if (*finished) {
			return;
		}
		*finished = true;
		if (self != nullptr) {
			self->m_busy = false;
		}
		socket->deleteLater();
		done(reply, error);
	};

	connect(socket, &QTcpSocket::connected, socket,
	        [socket, command] { socket->write((command + QLatin1Char('\n')).toUtf8()); });
	connect(socket, &QTcpSocket::readyRead, socket, [socket, buffer] { buffer->append(socket->readAll()); });
	connect(socket, &QTcpSocket::disconnected, socket, [socket, buffer, finish] {
		buffer->append(socket->readAll());
		QJsonParseError parse {};
		const auto      document = QJsonDocument::fromJson(*buffer, &parse);
		if (parse.error != QJsonParseError::NoError || !document.isObject()) {
			finish({}, tr("bad reply: %1").arg(parse.errorString()));
			return;
		}
		const auto reply = document.object();
		if (reply.contains(QStringLiteral("error"))) {
			finish(reply, reply.value(QStringLiteral("error")).toString());
			return;
		}
		finish(reply, {});
	});
	connect(socket, &QTcpSocket::errorOccurred, socket, [socket, finish](QAbstractSocket::SocketError error) {
		// The server closes the connection after replying; that arrives as RemoteHostClosedError
		// and is handled by disconnected().
		if (error == QAbstractSocket::RemoteHostClosedError) {
			return;
		}
		finish({}, error == QAbstractSocket::ConnectionRefusedError
		               ? tr("emulator not running (no debug server on port %1)").arg(KYTY_DEBUG_SERVER_PORT)
		               : socket->errorString());
	});
	// A large snapshot (tens of thousands of memory ranges) can take a moment to serialize.
	QTimer::singleShot(10000, socket, [finish] { finish({}, tr("timed out")); });

	socket->connectToHost(QStringLiteral("127.0.0.1"), KYTY_DEBUG_SERVER_PORT);
}
