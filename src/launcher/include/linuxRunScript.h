#ifndef LINUX_RUN_SCRIPT_H
#define LINUX_RUN_SCRIPT_H

#include <QString>
#include <QStringList>

class QProcess;

namespace LinuxRunScript {

bool Prepare(QProcess* process, const QString& interpreter, const QStringList& args,
             QString* script_file);

}

#endif
