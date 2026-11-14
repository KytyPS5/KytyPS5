#ifndef CMD_COMMAND_H
#define CMD_COMMAND_H

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <optional>

struct CmdLaunch {
	QString             native_arguments;
	QProcessEnvironment environment;
};

inline constexpr char      CmdProgram[]      = "cmd.exe";
inline constexpr qsizetype CmdArgumentsLimit = 8191;

inline QString CrtQuotedContent(const QString& value) {
	QString   escaped;
	qsizetype backslashes = 0;
	for (const QChar c: value) {
		if (c == u'\\') {
			backslashes++;
			continue;
		}
		const qsizetype doubled = c == u'"' ? backslashes * 2 + 1 : backslashes;
		escaped.append(QString(doubled, u'\\')).append(c);
		backslashes = 0;
	}
	return escaped.append(QString(backslashes * 2, u'\\'));
}

inline std::optional<CmdLaunch> CmdCommand(const QStringList& tokens, bool close_on_clean_exit,
                                           QProcessEnvironment environment) {
	constexpr qsizetype separator_and_quotes = 3;
	QStringList         references;
	qsizetype           expanded_arguments = 0;
	for (qsizetype i = 0; i < tokens.size(); i++) {
		if (tokens[i].contains(QChar(u'\0'))) {
			return std::nullopt;
		}
		const QString content = CrtQuotedContent(tokens[i]);
		if (i > 0) {
			expanded_arguments += content.size() + separator_and_quotes;
		}
		if (content.isEmpty()) {
			references.append(QStringLiteral("\"\""));
			continue;
		}
		const QString name = QStringLiteral("KYTY_ARG_%1").arg(i);
		environment.insert(name, content);
		references.append(QStringLiteral("\"!%1!\"").arg(name));
	}
	const QString exit_on_success = close_on_clean_exit ? QStringLiteral(" && exit") : QString();
	const QString native_arguments =
	    QStringLiteral("/V:ON /K \"") + references.join(u' ') + exit_on_success + u'"';
	constexpr qsizetype program_and_separator = sizeof(CmdProgram);
	if (expanded_arguments > CmdArgumentsLimit ||
	    program_and_separator + native_arguments.size() > CmdArgumentsLimit) {
		return std::nullopt;
	}
	return CmdLaunch {native_arguments, environment};
}

#endif
