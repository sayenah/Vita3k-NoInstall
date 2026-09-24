// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <module/module.h>

#include <ime/functions.h>
#include <ime/types.h>
#include <kernel/state.h>

#include <atomic>
#include <mutex>
#include <vector>

#include <util/lock_and_find.h>

#ifdef __ANDROID__
#include <ime/keyboard.h>
#endif

#include <util/tracy.h>
TRACY_MODULE_NAME(SceIme);

EXPORT(void, SceImeEventHandler, Ptr<void> arg, const SceImeEvent *e) {
    TRACY_FUNC(SceImeEventHandler, arg, e);
    Ptr<SceImeEvent> e1 = Ptr<SceImeEvent>(alloc(emuenv.mem, sizeof(SceImeEvent), "ime2"));
    memcpy(e1.get(emuenv.mem), e, sizeof(SceImeEvent));
    auto thread = emuenv.kernel.get_thread(thread_id);
    thread->run_callback(emuenv.ime.param.handler.address(), { arg.address(), e1.address() });
    free(emuenv.mem, e1.address());
}

EXPORT(SceInt32, sceImeClose) {
    TRACY_FUNC(sceImeClose);
    emuenv.ime.state = false;

    if (emuenv.ime.param.inputTextBuffer.address())
        free(emuenv.mem, emuenv.ime.param.inputTextBuffer.address());
    emuenv.ime.param.inputTextBuffer = Ptr<SceWChar16>();

#ifdef __ANDROID__
    ime::set_keyboard_active(false);
#endif

    return 0;
}

EXPORT(SceInt32, sceImeOpen, SceImeParam *param) {
    TRACY_FUNC(sceImeOpen, param);
    emuenv.ime.caps_level = 0;
    emuenv.ime.caretIndex = 0;
    emuenv.ime.edit_text = {};
    emuenv.ime.enter_label.clear();
    emuenv.ime.str.clear();
    emuenv.ime.param = *param;

    switch (emuenv.ime.param.enterLabel) {
    case SCE_IME_ENTER_LABEL_DEFAULT:
        emuenv.ime.enter_label = "Enter";
        break;
    case SCE_IME_ENTER_LABEL_SEND:
        emuenv.ime.enter_label = "Send";
        break;
    case SCE_IME_ENTER_LABEL_SEARCH:
        emuenv.ime.enter_label = "Search";
        break;
    case SCE_IME_ENTER_LABEL_GO:
        emuenv.ime.enter_label = "Go";
        break;
    default: break;
    }

    emuenv.ime.edit_text.str = emuenv.ime.param.inputTextBuffer;
    emuenv.ime.param.inputTextBuffer = Ptr<SceWChar16>(alloc(emuenv.mem, (SCE_IME_MAX_PREEDIT_LENGTH + emuenv.ime.param.maxTextLength + 1) * sizeof(SceWChar16), "ime_str"));
    emuenv.ime.str = emuenv.ime.param.initialText ? reinterpret_cast<char16_t *>(emuenv.ime.param.initialText.get(emuenv.mem)) : u"";
    if (!emuenv.ime.str.empty())
        emuenv.ime.caretIndex = emuenv.ime.edit_text.caretIndex = emuenv.ime.edit_text.preeditIndex = static_cast<SceUInt32>(emuenv.ime.str.length());
    else
        emuenv.ime.caps_level = 1;

    emuenv.ime.events.clear();
    emuenv.ime.push_event(SCE_IME_EVENT_OPEN);
    emuenv.ime.state = true;

#ifdef __ANDROID__
    ime::set_keyboard_active(true);
#endif

    return 0;
}

EXPORT(SceInt32, sceImeSetCaret, const SceImeCaret *caret) {
    TRACY_FUNC(sceImeSetCaret, caret);
    if (!caret)
        return RET_ERROR(SCE_IME_ERROR_INVALID_POINTER);
    if (!emuenv.ime.state)
        return RET_ERROR(SCE_IME_ERROR_NOT_OPENED);

    // store the caret and queue the event, instead of running the guest handler from inside a setter
    std::lock_guard lock(emuenv.ime.mutex);
    emuenv.ime.caretIndex = caret->index;
    emuenv.ime.edit_text.caretIndex = caret->index;
    emuenv.ime.push_event(SCE_IME_EVENT_UPDATE_CARET);

    return 0;
}

EXPORT(SceInt32, sceImeSetPreeditGeometry, const SceImePreeditGeometry *preedit) {
    TRACY_FUNC(sceImeSetPreeditGeometry, preedit);
    if (!preedit)
        return RET_ERROR(SCE_IME_ERROR_INVALID_POINTER);
    if (!emuenv.ime.state)
        return RET_ERROR(SCE_IME_ERROR_NOT_OPENED);

    std::lock_guard lock(emuenv.ime.mutex);
    emuenv.ime.preedit_rect.x = preedit->x;
    emuenv.ime.preedit_rect.y = preedit->y;
    emuenv.ime.preedit_rect.height = preedit->height;
    emuenv.ime.push_event(SCE_IME_EVENT_CHANGE_SIZE);

    return 0;
}

EXPORT(SceInt32, sceImeSetText, const SceWChar16 *text, SceUInt32 length) {
    TRACY_FUNC(sceImeSetText, text, length);
    if (!text)
        return RET_ERROR(SCE_IME_ERROR_INVALID_POINTER);
    if (!emuenv.ime.state)
        return RET_ERROR(SCE_IME_ERROR_NOT_OPENED);
    if (length > emuenv.ime.param.maxTextLength)
        return RET_ERROR(SCE_IME_ERROR_INVALID_PARAM);

    std::u16string new_text;
    new_text.reserve(length);
    for (SceUInt32 i = 0; i < length; i++) {
        const char16_t c = static_cast<char16_t>(text[i]);
        if (c == 0x0A || c == 0x0D || (c >= 0xD800 && c <= 0xDFFF))
            return RET_ERROR(SCE_IME_ERROR_INVALID_TEXT);
        if (c == 0)
            break;
        new_text.push_back(c);
    }

    std::lock_guard lock(emuenv.ime.mutex);
    emuenv.ime.str = new_text;
    emuenv.ime.caretIndex = static_cast<uint32_t>(new_text.length());
    emuenv.ime.edit_text.caretIndex = emuenv.ime.caretIndex;
    emuenv.ime.edit_text.preeditIndex = emuenv.ime.caretIndex;
    emuenv.ime.edit_text.preeditLength = 0;
    emuenv.ime.edit_text.editIndex = 0;
    emuenv.ime.edit_text.editLengthChange = 0;
    emuenv.ime.push_event(SCE_IME_EVENT_UPDATE_TEXT);

    return 0;
}

EXPORT(SceInt32, sceImeUpdate) {
    TRACY_FUNC(sceImeUpdate);
    if (!emuenv.ime.state)
        return RET_ERROR(SCE_IME_ERROR_NOT_OPENED);

    std::vector<uint32_t> pending;
    SceImeEditText text{};
    SceImeRect rect{};
    std::u16string str;
    uint32_t caret = 0;
    {
        std::lock_guard lock(emuenv.ime.mutex);
        if (emuenv.ime.events.empty())
            return 0;
        pending.assign(emuenv.ime.events.begin(), emuenv.ime.events.end());
        emuenv.ime.events.clear();
        text = emuenv.ime.edit_text;
        rect = emuenv.ime.preedit_rect;
        str = emuenv.ime.str;
        caret = emuenv.ime.caretIndex;
    }

    if (text.str)
        memcpy(text.str.get(emuenv.mem), str.c_str(), (str.length() + 1) * sizeof(SceWChar16));

    static std::atomic<uint32_t> delivered{ 0 };
    for (const uint32_t id : pending) {
        // SceImeEventParam is a UNION: the old code wrote param.text and then param.caretIndex, which landed on top
        // of text.preeditIndex. Each event fills only the member it owns (seq-256).
        SceImeEvent e{};
        e.id = id;
        switch (id) {
        case SCE_IME_EVENT_UPDATE_TEXT:
            e.param.text = text;
            break;
        case SCE_IME_EVENT_UPDATE_CARET:
            e.param.caretIndex = caret;
            break;
        case SCE_IME_EVENT_CHANGE_SIZE:
            e.param.rect = rect;
            break;
        default:
            // OPEN, PRESS_ENTER and PRESS_CLOSE carry no parameter
            break;
        }

        const uint32_t n = delivered.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16 || (n & 255) == 0)
            LOG_DEBUG("[IME] event #{} id {} caret {} text length {}", n, id, caret, str.length());

        CALL_EXPORT(SceImeEventHandler, emuenv.ime.param.arg, &e);
    }

    return 0;
}
