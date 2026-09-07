#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <string_view>

namespace md::native {
struct RichLoad { bool editable = false; std::wstring reason; };
// Unsupported input never replaces the control's current contents.
RichLoad load(HWND rich, std::string_view markdown, bool dark = false);
// Failure leaves markdown unchanged. Selection and scroll position are preserved.
bool save(HWND rich, std::string& markdown, std::wstring& error);
void toggleInline(HWND rich, int style);
void setBlock(HWND rich, int style);
void restyle(HWND rich, bool dark);
// Handles Enter only inside one code paragraph. False means caller may apply
// the ordinary RichEdit Enter behavior. Successful insertion is undoable.
bool insertCodeLineBreak(HWND rich);
}
