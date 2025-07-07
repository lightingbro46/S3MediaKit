#ifndef S3MANAGERKIT_MANAGER_H
#define S3MANAGERKIT_MANAGER_H

#include <string>
#include <functional>
#include "json/json.h"

void installManagerHook();

void unInstallManagerHook();

void migrateDatabase();

#endif // S3MANAGERKIT_MANAGER_H
