#ifndef RUNTIME_FAULTS_H
#define RUNTIME_FAULTS_H

void runtime_install_fault_handlers(void);
void runtime_panic(const char *message);

#endif
