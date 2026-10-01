#ifndef DEBUG_CLIENT_H
#define DEBUG_CLIENT_H

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

// Localhost port the launcher passes to the emulator as --debug-server-port.
constexpr quint16 KYTY_DEBUG_SERVER_PORT = 47650;

// One request per connection: send a command line, read one JSON document back. The emulator's
// debug server answers only while a game is running.
class DebugClient final: public QObject {
public:
	using Callback = std::function<void(const QJsonObject& reply, const QString& error)>;

	explicit DebugClient(QObject* parent = nullptr): QObject(parent) {}

	// Fires `done` exactly once, on the GUI thread. A request already in flight makes this one
	// fail immediately, so a slow emulator cannot pile up polls.
	void Request(const QString& command, Callback done);

	[[nodiscard]] bool Busy() const { return m_busy; }

private:
	bool m_busy = false;
};

#endif // DEBUG_CLIENT_H
