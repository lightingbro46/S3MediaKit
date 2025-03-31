/*
 * Copyright (c) 2025-present The S3MediaKit project authors. All Rights Reserved.
 *
 * This file is part of S3MediaKit(https://github.com/S3MediaKit/S3MediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_SYSTEM_H
#define ZLMEDIAKIT_SYSTEM_H

#include <string>

class System {
public:
    static std::string execute(const std::string &cmd);
    static void startDaemon(bool &kill_parent_if_failed);
    static void systemSetup();
};

#endif //ZLMEDIAKIT_SYSTEM_H
