/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace Web {

// The payload of a drag that started in a page. WebContent sends it to the UI process, which hands it to the
// platform's drag source. A text item carries its UTF-8 text; a file item carries the file's contents and name.
struct DragDataItem {
    enum class Kind : u8 {
        Text,
        File,
    };

    Kind kind { Kind::Text };
    String type;
    String text;
    ByteBuffer file_data;
    String file_name;
};

struct DragData {
    Vector<DragDataItem> items;
};

}

#include <LibWebCommon/Page/DragDataCodecs.h>
