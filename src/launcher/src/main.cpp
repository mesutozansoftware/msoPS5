#include "mainDialog.h"
#include "launcherTheme.h"

#include <QApplication>
#include <QMessageBox>
#include <QObject>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#if defined(__APPLE__)
// msoPS5 supports Apple Silicon only, where this x86_64 binary runs under Rosetta 2.
static bool IsRunningUnderRosetta() {
	int    translated = 0;
	size_t size       = sizeof(translated);
	return ::sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 &&
	       translated == 1;
}
#endif

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);
	LauncherTheme::Initialize(a);

#if defined(__APPLE__)
	if (!IsRunningUnderRosetta()) {
		QMessageBox::critical(nullptr, QObject::tr("Unsupported Mac"),
		                      QObject::tr("msoPS5 requires a Mac with Apple silicon (M1 or later)."));
		return 1;
	}
#endif

	MainDialog w;

	w.emit Start();

	w.show();

	return QApplication::exec();
}
