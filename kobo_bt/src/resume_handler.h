#ifndef RESUME_HANDLER_H
#define RESUME_HANDLER_H

#include <QObject>

class ResumeHandler : public QObject {
    Q_OBJECT
public slots:
    void onResume();
};

#endif
