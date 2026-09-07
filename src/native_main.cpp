#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <psapi.h>
#include <imm.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "Scintilla.h"
#include "SciLexer.h"
#include "document.hpp"
#include "preview.hpp"
#include "native_rich.hpp"
#include "resource.h"
#include "scroll_motion.hpp"

namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t PreviewLimit = 128 * 1024;
constexpr size_t LargeFileLimit = 2 * 1024 * 1024;
constexpr UINT PreviewReady = WM_APP + 1;
constexpr UINT_PTR PreviewTimer = 1;
constexpr UINT_PTR StatusTimer = 2;
constexpr wchar_t WindowClass[] = L"Qingmo.Native.Window";
enum Command { New = 1001, Open, Save, SaveAs, Find, FindNext, FindPrevious,
    TogglePreview, Refresh, ToggleWrap, ToggleTheme, ToggleSync, About,
    Bold, Italic, InlineCode, FocusMode, More, CloseFind,
    EditMode, SourceMode, ReadMode, FormatMenu, FontMenu, Strike,
    FontLarger, FontSmaller, FontSans, FontSerif, WidePage, ToggleFiles, OpenFolder, BlockBase=1100 };
enum class Mode { Rich, Source, Read };
struct Palette { COLORREF bg, paper, ink, muted, border, accent, hover; };
Palette colors(bool dark) {
    return dark ? Palette{RGB(29,31,33),RGB(33,35,37),RGB(222,224,223),RGB(146,155,151),RGB(56,61,59),RGB(119,193,171),RGB(48,54,51)}
                : Palette{RGB(246,247,244),RGB(253,253,251),RGB(43,51,47),RGB(124,135,127),RGB(225,229,221),RGB(39,114,86),RGB(232,239,231)};
}
double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count);
    return result;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(count,'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count,nullptr,nullptr);
    return result;
}
std::wstring windowText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(length+1,L'\0');
    text.resize(GetWindowTextW(hwnd,text.data(),length+1));
    return text;
}
std::wstring byteLabel(size_t bytes) {
    wchar_t value[64];
    if (bytes < 1024) swprintf(value,64,L"%zu B",bytes);
    else if (bytes < 1024*1024) swprintf(value,64,L"%.1f KiB",bytes/1024.0);
    else swprintf(value,64,L"%.1f MiB",bytes/(1024.0*1024.0));
    return value;
}
struct PreviewJob { uint64_t version; std::string text; md::PreviewOptions options; bool truncated; };
struct PreviewOutput { uint64_t version; md::PreviewResult result; double milliseconds; bool truncated;
    std::string source; md::PreviewOptions options; };
struct Stream { const std::string* bytes; size_t offset = 0; };
DWORD CALLBACK streamRtf(DWORD_PTR cookie, LPBYTE data, LONG requested, LONG* written) {
    auto& stream = *reinterpret_cast<Stream*>(cookie);
    const size_t count = std::min<size_t>(requested,stream.bytes->size()-stream.offset);
    memcpy(data,stream.bytes->data()+stream.offset,count);
    stream.offset += count;
    *written = static_cast<LONG>(count);
    return 0;
}

class App {
public:
    HWND hwnd = nullptr, editor = nullptr, preview = nullptr, search = nullptr;
    HINSTANCE instance = nullptr;
    HMODULE scintillaModule = nullptr, lexillaModule = nullptr, richModule = nullptr;
    SciFnDirect direct = nullptr;
    sptr_t directPointer = 0;
    HFONT uiFont = nullptr, smallFont = nullptr, titleFont = nullptr;
    HBRUSH paperBrush = nullptr;
    UINT dpi = 96;
    bool dark = false, previewShown = true, wrap = true, syncScroll = false;
    bool focusMode = false, findShown = false, loading = false, largeFile = false;
    bool dragging = false, shuttingDown = false, automated = false;
    Mode mode = Mode::Rich, previousMode = Mode::Rich;
    bool richCurrent = false, richDirty = false, richEditable = true, widePage = false;
    int zoom = 100;
    std::wstring richReason, findResult;
    std::string richOriginal, richBaseline;
    WINDOWPLACEMENT windowedPlacement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowedStyle = 0;
    HWND focusExit = nullptr;
    HWND fileList = nullptr, folderButton = nullptr;
    bool filesShown = true;
    int filesWidth = 210;
    std::wstring folderPath;
    struct FileEntry { std::wstring path; bool directory; };
    std::vector<FileEntry> files;
    md::ScalarScrollMotion scrollMotion;
    double split = 0.49;
    int splitX = 0, editorTop = 0, contentBottom = 0;
    RECT client{};
    std::wstring path, settingsPath, notice;
    md::Encoding encoding = md::Encoding::Utf8;
    md::FileStamp stamp;
    std::vector<HWND> toolbar;
    HWND searchNext = nullptr, searchPrev = nullptr, searchClose = nullptr;
    std::atomic<uint64_t> version{0};
    uint64_t displayedVersion = UINT64_MAX;
    bool displayedTruncated = false;
    double parseMs = 0, streamMs = 0;
    bool hasAppliedPreview = false;
    std::string appliedSource;
    md::PreviewOptions appliedOptions;
    std::mutex workerMutex;
    std::condition_variable workerWake;
    bool stopWorker = false;
    std::optional<PreviewJob> pending;
    std::optional<PreviewOutput> output;
    std::thread worker;

    explicit App(HINSTANCE h, bool test) : instance(h), automated(test) {}
    ~App() {
        stop();
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
        if (uiFont) DeleteObject(uiFont);
        if (smallFont) DeleteObject(smallFont);
        if (titleFont) DeleteObject(titleFont);
        if (paperBrush) DeleteObject(paperBrush);
        if (richModule) FreeLibrary(richModule);
        if (lexillaModule) FreeLibrary(lexillaModule);
        if (scintillaModule) FreeLibrary(scintillaModule);
    }
    sptr_t sci(unsigned int message, uptr_t w = 0, sptr_t l = 0) const {
        return direct ? direct(directPointer,message,w,l) : 0;
    }
    int px(int logical) const { return MulDiv(logical,dpi,96); }
    bool dirty() const { return richDirty || sci(SCI_GETMODIFY) != 0; }
    HWND activeEditor() const { return mode==Mode::Source?editor:preview; }
    void error(const std::wstring& message) {
        MessageBoxW(hwnd,message.c_str(),L"轻墨",MB_OK|MB_ICONERROR);
    }
    void flash(std::wstring text) {
        notice = std::move(text);
        SetTimer(hwnd,StatusTimer,3500,nullptr);
        invalidateStatus();
    }
    void invalidateStatus() {
        RECT rect{0,client.bottom-px(30),client.right,client.bottom};
        InvalidateRect(hwnd,&rect,FALSE);
    }
    void invalidateHeader() {
        RECT rect{0,0,client.right,editorTop};
        InvalidateRect(hwnd,&rect,FALSE);
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(workerMutex); stopWorker=true; pending.reset(); }
        workerWake.notify_one();
        if (worker.joinable()) worker.join();
    }
    void startWorker() {
        worker = std::thread([this] {
            while (true) {
                PreviewJob job;
                {
                    std::unique_lock<std::mutex> lock(workerMutex);
                    workerWake.wait(lock,[this] { return stopWorker || pending.has_value(); });
                    if (stopWorker) return;
                    job = std::move(*pending); pending.reset();
                }
                const auto started = Clock::now();
                md::PreviewResult result;
                try { result = md::renderMarkdown(job.text,job.options); }
                catch (...) { result.ok=false; }
                const double duration = elapsedMs(started);
                {
                    std::lock_guard<std::mutex> lock(workerMutex);
                    if (stopWorker) return;
                    if (job.version != version.load()) continue;
                    output = PreviewOutput{job.version,std::move(result),duration,job.truncated,std::move(job.text),job.options};
                }
                PostMessageW(hwnd,PreviewReady,0,0);
            }
        });
    }
    HWND button(const wchar_t* label, int id) {
        return CreateWindowExW(0,L"BUTTON",label,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
            0,0,1,1,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
    }
    bool initialize() {
        wchar_t modulePath[32768]{};
        GetModuleFileNameW(nullptr,modulePath,32768);
        const auto directory = std::filesystem::path(modulePath).parent_path();
        scintillaModule = LoadLibraryExW((directory/L"Scintilla.dll").c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        lexillaModule = LoadLibraryExW((directory/L"Lexilla.dll").c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        richModule = LoadLibraryExW(L"Msftedit.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!scintillaModule || !lexillaModule || !richModule) {
            error(L"无法加载编辑组件。请将 QingmoNative.exe、Scintilla.dll 和 Lexilla.dll 放在同一个文件夹。");
            return false;
        }
        if (!automated) {
            wchar_t local[32768]{};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768)) {
                const auto settingsDirectory = std::filesystem::path(local)/L"QingmoNative";
                settingsPath = (settingsDirectory/L"settings.ini").wstring();
                dark = GetPrivateProfileIntW(L"view",L"dark",0,settingsPath.c_str()) != 0;
                wrap = GetPrivateProfileIntW(L"view",L"wrap",1,settingsPath.c_str()) != 0;
                previewShown = GetPrivateProfileIntW(L"view",L"preview",1,settingsPath.c_str()) != 0;
                syncScroll = GetPrivateProfileIntW(L"view",L"sync",0,settingsPath.c_str()) != 0;
                split = std::clamp(GetPrivateProfileIntW(L"view",L"split",49,settingsPath.c_str())/100.0,0.25,0.75);
                zoom = std::clamp<int>(GetPrivateProfileIntW(L"view",L"zoom",100,settingsPath.c_str()),80,160);
                widePage = GetPrivateProfileIntW(L"view",L"wide",0,settingsPath.c_str())!=0;
                filesShown = GetPrivateProfileIntW(L"view",L"files",1,settingsPath.c_str())!=0;
                filesWidth = std::clamp<int>(GetPrivateProfileIntW(L"view",L"filesWidth",210,settingsPath.c_str()),150,360);
                wchar_t lastFolder[32768]{}; GetPrivateProfileStringW(L"view",L"folder",L"",lastFolder,32768,settingsPath.c_str()); folderPath=lastFolder;
            }
        }
        WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc);
        wc.hInstance=instance; wc.lpfnWndProc=windowProc; wc.lpszClassName=WindowClass;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDR_APP_ICON));
        wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDR_APP_ICON),IMAGE_ICON,16,16,LR_DEFAULTCOLOR));
        RegisterClassExW(&wc);
        dpi = GetDpiForSystem();
        hwnd = CreateWindowExW(WS_EX_ACCEPTFILES,WindowClass,L"轻墨",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
            CW_USEDEFAULT,CW_USEDEFAULT,px(1180),px(790),nullptr,nullptr,instance,this);
        if (!hwnd) return false;
        editor = CreateWindowExW(0,L"Scintilla",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|WS_HSCROLL,
            0,0,1,1,hwnd,reinterpret_cast<HMENU>(10),instance,nullptr);
        preview = CreateWindowExW(0,MSFTEDIT_CLASS,L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_TABSTOP|
            ES_MULTILINE|ES_READONLY|ES_NOHIDESEL,0,0,1,1,hwnd,reinterpret_cast<HMENU>(11),instance,nullptr);
        if (!editor || !preview) return false;
        direct = reinterpret_cast<SciFnDirect>(SendMessageW(editor,SCI_GETDIRECTFUNCTION,0,0));
        directPointer = SendMessageW(editor,SCI_GETDIRECTPOINTER,0,0);
        sci(SCI_SETCODEPAGE,SC_CP_UTF8);
        sci(SCI_SETTECHNOLOGY,SC_TECHNOLOGY_DIRECTWRITE);
        sci(SCI_SETIMEINTERACTION,SC_IME_INLINE);
        sci(SCI_SETLAYOUTCACHE,SC_CACHE_PAGE);
        sci(SCI_SETLAYOUTTHREADS,2);
        sci(SCI_SETIDLESTYLING,SC_IDLESTYLING_TOVISIBLE);
        sci(SCI_SETSCROLLWIDTHTRACKING,1);
        sci(SCI_SETSCROLLWIDTH,1);
        sci(SCI_SETTABWIDTH,4);
        sci(SCI_SETUSETABS,0);
        sci(SCI_SETINDENT,4);
        sci(SCI_SETTABINDENTS,1);
        sci(SCI_SETBACKSPACEUNINDENTS,1);
        sci(SCI_SETEOLMODE,SC_EOL_LF);
        sci(SCI_SETMARGINTYPEN,0,SC_MARGIN_NUMBER);
        sci(SCI_SETMARGINWIDTHN,1,0);
        sci(SCI_SETMARGINWIDTHN,2,0);
        sci(SCI_SETMARGINLEFT,0,px(12));
        sci(SCI_SETMARGINRIGHT,0,px(20));
        sci(SCI_SETEXTRAASCENT,px(3));
        sci(SCI_SETEXTRADESCENT,px(3));
        sci(SCI_SETCARETWIDTH,2);
        sci(SCI_SETCARETLINEVISIBLE,1);
        sci(SCI_SETMODEVENTMASK,SC_MOD_INSERTTEXT|SC_MOD_DELETETEXT);
        SendMessageW(preview,EM_SETUNDOLIMIT,500,0);
        SendMessageW(preview,EM_EXLIMITTEXT,0,16*1024*1024);
        SendMessageW(preview,EM_SETEVENTMASK,0,ENM_SCROLL|ENM_CHANGE|ENM_SELCHANGE);
        SetWindowSubclass(preview,richProc,1,reinterpret_cast<DWORD_PTR>(this));
        search = CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,
            0,0,1,1,hwnd,reinterpret_cast<HMENU>(20),instance,nullptr);
        SendMessageW(search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"查找文本…"));
        SendMessageW(search,EM_SETLIMITTEXT,4096,0);
        fileList=CreateWindowExW(0,L"LISTBOX",L"文件列表",WS_CHILD|WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
            0,0,1,1,hwnd,reinterpret_cast<HMENU>(30),instance,nullptr);
        folderButton=button(L"打开文件夹…",OpenFolder);
        toolbar = {button(L"文件",ToggleFiles),button(L"编辑",EditMode),button(L"源码",SourceMode),button(L"阅读",ReadMode),
            button(L"打开",Open),button(L"保存",Save),button(L"格式",FormatMenu),button(L"排版",FontMenu),button(L"更多",More)};
        focusExit=button(L"退出 · F11",FocusMode);
        searchPrev=button(L"上一处",FindPrevious); searchNext=button(L"下一处",FindNext); searchClose=button(L"关闭",CloseFind);
        updateFonts();
        configureDocument(0);
        applyTheme();
        layout();
        setDocument({},{});
        if (!folderPath.empty()) listFolder(folderPath);
        updateTitle();
        return true;
    }
    void updateFonts() {
        if (uiFont) DeleteObject(uiFont);
        if (smallFont) DeleteObject(smallFont);
        if (titleFont) DeleteObject(titleFont);
        uiFont=CreateFontW(-px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        smallFont=CreateFontW(-px(11),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        titleFont=CreateFontW(-px(14),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        if (search) SendMessageW(search,WM_SETFONT,reinterpret_cast<WPARAM>(uiFont),TRUE);
        if (fileList) SendMessageW(fileList,WM_SETFONT,reinterpret_cast<WPARAM>(uiFont),TRUE);
    }
    void applyTheme() {
        const auto p = colors(dark);
        if (paperBrush) DeleteObject(paperBrush);
        paperBrush=CreateSolidBrush(p.paper);
        const BOOL enabled=dark;
        DwmSetWindowAttribute(hwnd,20,&enabled,sizeof(enabled));
        sci(SCI_STYLESETFONT,STYLE_DEFAULT,reinterpret_cast<sptr_t>("Consolas"));
        sci(SCI_STYLESETSIZE,STYLE_DEFAULT,12);
        sci(SCI_STYLESETFORE,STYLE_DEFAULT,p.ink);
        sci(SCI_STYLESETBACK,STYLE_DEFAULT,p.paper);
        sci(SCI_STYLECLEARALL);
        sci(SCI_STYLESETFORE,STYLE_LINENUMBER,p.muted);
        sci(SCI_STYLESETBACK,STYLE_LINENUMBER,p.paper);
        sci(SCI_STYLESETSIZE,STYLE_LINENUMBER,10);
        sci(SCI_SETCARETFORE,p.accent);
        sci(SCI_SETCARETLINEBACK,p.hover);
        sci(SCI_SETSELFORE,TRUE,p.ink);
        sci(SCI_SETSELBACK,TRUE,dark?RGB(63,86,73):RGB(215,235,218));
        for (int s=SCE_MARKDOWN_HEADER1;s<=SCE_MARKDOWN_HEADER6;++s) {
            sci(SCI_STYLESETFORE,s,p.accent); sci(SCI_STYLESETBOLD,s,1);
        }
        for (int s : {SCE_MARKDOWN_STRONG1,SCE_MARKDOWN_STRONG2}) sci(SCI_STYLESETBOLD,s,1);
        for (int s : {SCE_MARKDOWN_EM1,SCE_MARKDOWN_EM2}) sci(SCI_STYLESETITALIC,s,1);
        for (int s : {SCE_MARKDOWN_CODE,SCE_MARKDOWN_CODE2,SCE_MARKDOWN_CODEBK}) {
            sci(SCI_STYLESETFORE,s,dark?RGB(202,177,135):RGB(137,91,37)); sci(SCI_STYLESETBACK,s,p.bg);
        }
        sci(SCI_STYLESETFORE,SCE_MARKDOWN_LINK,p.accent);
        sci(SCI_STYLESETFORE,SCE_MARKDOWN_BLOCKQUOTE,p.muted);
        sci(SCI_STYLESETFORE,SCE_MARKDOWN_ULIST_ITEM,p.accent);
        sci(SCI_STYLESETFORE,SCE_MARKDOWN_OLIST_ITEM,p.accent);
        SendMessageW(preview,EM_SETBKGNDCOLOR,0,p.paper);
        if (richCurrent && richEditable) { loading=true; md::native::restyle(preview,dark); loading=false; }
        else if (richCurrent && !richEditable) { richCurrent=false; ensureRich(); }
        layout();
        RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
    }
    void configureDocument(size_t bytes) {
        largeFile=bytes>=LargeFileLimit;
        using CreateLexerFn = void* (__cdecl *)(const char*);
        auto createLexer=reinterpret_cast<CreateLexerFn>(GetProcAddress(lexillaModule,"CreateLexer"));
        sci(SCI_SETILEXER,0,reinterpret_cast<sptr_t>(largeFile?nullptr:(createLexer?createLexer("markdown"):nullptr)));
        sci(SCI_SETWRAPMODE,wrap && !largeFile?SC_WRAP_CHAR:SC_WRAP_NONE);
        updateMargin();
    }
    void updateMargin() {
        auto lines=sci(SCI_GETLINECOUNT);
        int digits=1; for (auto n=lines;n>=10;n/=10) ++digits;
        sci(SCI_SETMARGINWIDTHN,0,px(20+std::max(digits,3)*8));
    }
    void layout() {
        if (!hwnd) return;
        GetClientRect(hwnd,&client);
        const int top=focusMode?0:px(44), findHeight=findShown?px(42):0;
        editorTop=top+findHeight;
        contentBottom=std::max<int>(editorTop,client.bottom-(focusMode?0:px(28)));
        previewShown=mode!=Mode::Source;
        ShowWindow(editor,mode==Mode::Source?SW_SHOWNA:SW_HIDE);
        ShowWindow(preview,previewShown?SW_SHOWNA:SW_HIDE);
        splitX=filesShown && !focusMode?std::min(px(filesWidth),static_cast<int>(client.right)-px(360)):0;
        if (splitX<0) splitX=0;
        ShowWindow(fileList,splitX?SW_SHOWNA:SW_HIDE); ShowWindow(folderButton,splitX?SW_SHOWNA:SW_HIDE);
        MoveWindow(folderButton,px(8),editorTop+px(7),std::max(1,splitX-px(16)),px(31),TRUE);
        MoveWindow(fileList,px(8),editorTop+px(44),std::max(1,splitX-px(16)),std::max(1,contentBottom-editorTop-px(50)),TRUE);
        if (editor) MoveWindow(editor,splitX,editorTop,client.right-splitX,std::max(1,contentBottom-editorTop),TRUE);
        if (preview) {
            const int available=client.right-splitX;
            MoveWindow(preview,splitX,editorTop,available,std::max(1,contentBottom-editorTop),TRUE);
            const int width=std::min<int>(available-px(48),px(widePage?1100:800));
            const int inset=std::max<int>(px(24),(available-width)/2);
            RECT rect{inset,px(24),available-inset,std::max(px(25),contentBottom-editorTop-px(24))};
            SendMessageW(preview,EM_SETRECT,0,reinterpret_cast<LPARAM>(&rect));
            SendMessageW(preview,EM_SETZOOM,zoom,100);
        }
        const int bw=px(52), gap=px(3);
        for (size_t i=0;i<toolbar.size();++i) {
            const int x=i<4?px(12)+static_cast<int>(i)*(bw+gap):client.right-px(12)-static_cast<int>(toolbar.size()-i)*(bw+gap);
            MoveWindow(toolbar[i],x,px(5),bw,px(34),TRUE);
            ShowWindow(toolbar[i],focusMode?SW_HIDE:SW_SHOWNA);
        }
        MoveWindow(focusExit,client.right-px(104),px(5),px(98),px(29),TRUE);
        ShowWindow(focusExit,focusMode && !findShown?SW_SHOWNA:SW_HIDE);
        if (search) {
            MoveWindow(search,px(18),top+px(9),std::max<int>(px(60),client.right-px(350)),px(25),TRUE);
            ShowWindow(search,findShown?SW_SHOWNA:SW_HIDE);
            MoveWindow(searchPrev,client.right-px(236),top+px(5),px(74),px(32),TRUE);
            MoveWindow(searchNext,client.right-px(158),top+px(5),px(74),px(32),TRUE);
            MoveWindow(searchClose,client.right-px(80),top+px(5),px(64),px(32),TRUE);
            for (auto child:{searchPrev,searchNext,searchClose}) ShowWindow(child,findShown?SW_SHOWNA:SW_HIDE);
        }
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    bool ensureRich() {
        if (richCurrent) return true;
        const auto bytes=static_cast<size_t>(sci(SCI_GETLENGTH));
        const auto text=textPrefix(256*1024);
        richOriginal=bytes<=256*1024?text:std::string{}; richBaseline.clear();
        loading=true;
        richEditable=false;
        if (bytes<=256*1024) {
            const auto result=md::native::load(preview,text,dark);
            richEditable=result.editable; richReason=result.reason;
            if (richEditable) {
                std::wstring problem;
                if (!md::native::save(preview,richBaseline,problem)) { richEditable=false; richReason=problem; }
            }
        } else richReason=L"大文档使用源码编辑；阅读仅显示前 128 KiB。";
        if (!richEditable) {
            md::PreviewOptions options; options.dark=dark; options.reading=true; options.fontSizeHalfPoints=26;
            options.widthTwips=std::max(1000,MulDiv(std::min<int>(client.right-px(48),px(800)),1440,dpi));
            auto rendered=md::renderMarkdown(textPrefix(PreviewLimit),options);
            Stream stream{&rendered.rtf}; EDITSTREAM input{reinterpret_cast<DWORD_PTR>(&stream),0,streamRtf};
            SendMessageW(preview,EM_STREAMIN,SF_RTF,reinterpret_cast<LPARAM>(&input));
        }
        SendMessageW(preview,EM_EMPTYUNDOBUFFER,0,0);
        SendMessageW(preview,EM_SETMODIFY,FALSE,0);
        richDirty=false; richCurrent=true;
        loading=false;
        return true;
    }
    bool syncSource() {
        if (!richDirty) return true;
        std::string value; std::wstring problem;
        if (!md::native::save(preview,value,problem)) { error(L"无法无损保存正文："+problem+L"\n请撤销最近的格式操作后重试。"); return false; }
        if (value==richBaseline) value=richOriginal;
        else {
            const int eol=static_cast<int>(sci(SCI_GETEOLMODE));
            if (eol!=SC_EOL_LF) {
                std::string converted;
                for (char ch:value) { if (ch=='\n') converted+=eol==SC_EOL_CRLF?"\r\n":"\r"; else converted+=ch; }
                value=std::move(converted);
            }
        }
        if (value!=textPrefix()) {
            loading=true;
            sci(SCI_BEGINUNDOACTION); sci(SCI_SETTARGETRANGE,0,sci(SCI_GETLENGTH));
            sci(SCI_REPLACETARGET,value.size(),reinterpret_cast<sptr_t>(value.data())); sci(SCI_ENDUNDOACTION);
            loading=false; configureDocument(value.size()); version.fetch_add(1);
        }
        richDirty=false; updateTitle();
        return true;
    }
    void setMode(Mode next) {
        if (next==Mode::Source && !syncSource()) return;
        if (next!=Mode::Source) {
            ensureRich();
            if (next==Mode::Rich && !richEditable) { next=Mode::Source; flash(L"此文档的复杂语法保留在源码中编辑；点击阅读可查看排版。"); }
        }
        if (next==Mode::Read && mode!=Mode::Read) previousMode=mode;
        mode=next;
        SendMessageW(preview,EM_SETREADONLY,mode!=Mode::Rich,0);
        layout(); SetFocus(activeEditor()); updateTitle();
    }
    void toggleFocus() {
        if (!focusMode) {
            windowedStyle=GetWindowLongPtrW(hwnd,GWL_STYLE);
            windowedPlacement.length=sizeof(windowedPlacement); GetWindowPlacement(hwnd,&windowedPlacement);
            MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
            focusMode=true;
            SetWindowLongPtrW(hwnd,GWL_STYLE,windowedStyle & ~(WS_OVERLAPPEDWINDOW|WS_MAXIMIZE));
            SetWindowPos(hwnd,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,
                monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_FRAMECHANGED|SWP_NOACTIVATE);
        } else {
            focusMode=false; SetWindowLongPtrW(hwnd,GWL_STYLE,windowedStyle);
            SetWindowPlacement(hwnd,&windowedPlacement);
            SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED|SWP_NOACTIVATE);
        }
        layout(); SetFocus(activeEditor());
    }
    void formattingMenu() {
        HMENU popup=CreatePopupMenu();
        AppendMenuW(popup,MF_STRING,Bold,L"粗体\tCtrl+B"); AppendMenuW(popup,MF_STRING,Italic,L"斜体\tCtrl+I");
        AppendMenuW(popup,MF_STRING,Strike,L"删除线"); AppendMenuW(popup,MF_STRING,InlineCode,L"行内代码");
        AppendMenuW(popup,MF_SEPARATOR,0,nullptr);
        const wchar_t* labels[]={L"正文",L"一级标题",L"二级标题",L"三级标题",L"四级标题",L"五级标题",L"六级标题",L"无序列表",L"有序列表",L"引用",L"代码块"};
        for (int i=0;i<11;++i) AppendMenuW(popup,MF_STRING,BlockBase+i,labels[i]);
        RECT rect{}; GetWindowRect(toolbar[6],&rect);
        int chosen=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_RIGHTALIGN,rect.right,rect.bottom,0,hwnd,nullptr); DestroyMenu(popup);
        if (chosen) command(chosen);
    }
    void typographyMenu() {
        HMENU popup=CreatePopupMenu();
        AppendMenuW(popup,MF_STRING,FontLarger,L"放大文字\tCtrl +"); AppendMenuW(popup,MF_STRING,FontSmaller,L"缩小文字\tCtrl -");
        AppendMenuW(popup,MF_STRING|(widePage?MF_CHECKED:0),WidePage,L"宽版正文");
        AppendMenuW(popup,MF_STRING|(dark?MF_CHECKED:0),ToggleTheme,L"深色主题");
        RECT rect{}; GetWindowRect(toolbar[7],&rect);
        int chosen=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_RIGHTALIGN,rect.right,rect.bottom,0,hwnd,nullptr); DestroyMenu(popup);
        if (chosen) command(chosen);
    }
    void listFolder(const std::wstring& directory) {
        std::error_code ec;
        std::filesystem::directory_iterator it(directory,std::filesystem::directory_options::skip_permission_denied,ec), end;
        if (ec) { flash(L"无法读取这个文件夹。"); return; }
        std::vector<FileEntry> next;
        int inspected=0;
        for (;it!=end && inspected<10000;it.increment(ec),++inspected) {
            if (ec) break;
            const bool dir=it->is_directory(ec); if (ec) { ec.clear(); continue; }
            auto ext=it->path().extension().wstring(); std::transform(ext.begin(),ext.end(),ext.begin(),towlower);
            if (dir || ext==L".md" || ext==L".markdown" || ext==L".mdown" || ext==L".txt") next.push_back({it->path().wstring(),dir});
        }
        std::sort(next.begin(),next.end(),[](const FileEntry& a,const FileEntry& b) { return a.directory!=b.directory?a.directory>b.directory:_wcsicmp(a.path.c_str(),b.path.c_str())<0; });
        folderPath=directory; files.clear();
        const auto parent=std::filesystem::path(directory).parent_path();
        if (!parent.empty() && parent!=std::filesystem::path(directory)) files.push_back({parent.wstring(),true});
        const bool hasParent=!files.empty(); files.insert(files.end(),next.begin(),next.end());
        SendMessageW(fileList,WM_SETREDRAW,FALSE,0); SendMessageW(fileList,LB_RESETCONTENT,0,0);
        for (size_t i=0;i<files.size();++i) {
            auto name=hasParent && i==0?L"↑ 上一级":(files[i].directory?L"[目录] ":L"  ")+std::filesystem::path(files[i].path).filename().wstring();
            SendMessageW(fileList,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
            if (!path.empty() && _wcsicmp(path.c_str(),files[i].path.c_str())==0) SendMessageW(fileList,LB_SETCURSEL,i,0);
        }
        const auto name=std::filesystem::path(directory).filename().wstring();
        SetWindowTextW(folderButton,(name.empty()?directory:name).c_str());
        SendMessageW(fileList,WM_SETREDRAW,TRUE,0); InvalidateRect(fileList,nullptr,TRUE);
        if (inspected>=10000) flash(L"文件夹较大，仅列出扫描到的前 10000 个条目；可用打开按钮选择其他文件。");
    }
    void chooseFolder() {
        IFileOpenDialog* dialog=nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return;
        DWORD options=0; dialog->GetOptions(&options); dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"选择 Markdown 文件夹");
        if (SUCCEEDED(dialog->Show(hwnd))) {
            IShellItem* result=nullptr;
            if (SUCCEEDED(dialog->GetResult(&result))) {
                PWSTR name=nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH,&name))) { listFolder(name); filesShown=true; layout(); CoTaskMemFree(name); }
                result->Release();
            }
        }
        dialog->Release();
    }
    void openListedFile() {
        const LRESULT selected=SendMessageW(fileList,LB_GETCURSEL,0,0);
        if (selected<0 || static_cast<size_t>(selected)>=files.size()) return;
        const auto entry=files[selected];
        if (entry.directory) listFolder(entry.path);
        else openPath(entry.path);
    }
    std::string textPrefix(size_t maximum=SIZE_MAX) const {
        const size_t length=static_cast<size_t>(sci(SCI_GETLENGTH));
        size_t count=std::min(length,maximum);
        if (count<length) {
            while (count && (sci(SCI_GETCHARAT,count)&0xc0)==0x80) --count;
        }
        std::string text(count+1,'\0');
        Sci_TextRangeFull range{{0,static_cast<Sci_Position>(count)},text.data()};
        sci(SCI_GETTEXTRANGEFULL,0,reinterpret_cast<sptr_t>(&range));
        text.resize(count); return text;
    }
    void queuePreview() {
        KillTimer(hwnd,PreviewTimer);
        if (mode!=Mode::Source && !richCurrent) ensureRich();
        displayedVersion=version.load();
    }
    void applyPreview() {
        std::optional<PreviewOutput> ready;
        { std::lock_guard<std::mutex> lock(workerMutex); ready=std::move(output); output.reset(); }
        if (!ready || ready->version!=version.load() || !previewShown) return;
        if (!ready->result.ok) { flash(L"预览暂时无法生成，原文仍可编辑和保存"); return; }
        POINT scroll{}; SendMessageW(preview,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
        const bool wasVisible=(GetWindowLongPtrW(preview,GWL_STYLE)&WS_VISIBLE)!=0;
        SendMessageW(preview,WM_SETREDRAW,FALSE,0);
        const auto started=Clock::now();
        Stream stream{&ready->result.rtf};
        EDITSTREAM editStream{reinterpret_cast<DWORD_PTR>(&stream),0,streamRtf};
        SendMessageW(preview,EM_STREAMIN,SF_RTF,reinterpret_cast<LPARAM>(&editStream));
        SendMessageW(preview,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
        if (wasVisible) SendMessageW(preview,WM_SETREDRAW,TRUE,0);
        InvalidateRect(preview,nullptr,FALSE);
        streamMs=elapsedMs(started);
        if (editStream.dwError) { flash(L"预览显示失败，原文仍可编辑和保存"); return; }
        parseMs=ready->milliseconds;
        appliedSource=std::move(ready->source); appliedOptions=ready->options; hasAppliedPreview=true;
        displayedVersion=ready->version; displayedTruncated=ready->truncated;
        invalidateHeader(); invalidateStatus();
    }
    void changed() {
        if (loading) return;
        richCurrent=false;
        version.fetch_add(1);
        const auto bytes=static_cast<size_t>(sci(SCI_GETLENGTH));
        if ((bytes>=LargeFileLimit)!=largeFile) configureDocument(bytes);
        invalidateStatus();
    }
    void setDocument(md::Document document, std::wstring nextPath) {
        loading=true;
        version.fetch_add(1);
        KillTimer(hwnd,PreviewTimer);
        SendMessageW(editor,WM_SETREDRAW,FALSE,0);
        sci(SCI_SETUNDOCOLLECTION,0);
        sci(SCI_CLEARALL);
        configureDocument(document.text.size());
        sci(SCI_ALLOCATE,document.text.size());
        sci(SCI_ADDTEXT,document.text.size(),reinterpret_cast<sptr_t>(document.text.data()));
        sci(SCI_EMPTYUNDOBUFFER);
        sci(SCI_SETUNDOCOLLECTION,1);
        sci(SCI_SETSAVEPOINT);
        sci(SCI_GOTOPOS,0);
        sci(SCI_SETXOFFSET,0);
        sci(SCI_SETEOLMODE,document.text.find("\r\n")!=std::string::npos?SC_EOL_CRLF:
            (document.text.find('\r')!=std::string::npos && document.text.find('\n')==std::string::npos?SC_EOL_CR:SC_EOL_LF));
        encoding=document.encoding; stamp=document.stamp; path=std::move(nextPath);
        loading=false;
        richCurrent=false; richDirty=false;
        ensureRich();
        mode=richEditable?Mode::Rich:Mode::Read;
        SendMessageW(preview,EM_SETREADONLY,mode!=Mode::Rich,0);
        layout();
        SendMessageW(editor,WM_SETREDRAW,TRUE,0);
        updateMargin(); updateTitle();
        POINT top{}; SendMessageW(preview,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&top));
        queuePreview();
        SetFocus(activeEditor());
        InvalidateRect(editor,nullptr,FALSE);
    }
    bool confirmDiscard() {
        if (!dirty()) return true;
        const int answer=MessageBoxW(hwnd,L"保存当前文档的修改？",L"轻墨",MB_YESNOCANCEL|MB_ICONQUESTION);
        if (answer==IDCANCEL) return false;
        return answer!=IDYES || save(false);
    }
    bool openPath(const std::wstring& nextPath, bool prompt=true) {
        if (prompt && !confirmDiscard()) return false;
        md::Document document; std::wstring problem;
        if (!md::loadDocument(nextPath,document,problem)) { if (!automated) error(problem); return false; }
        setDocument(std::move(document),std::filesystem::absolute(nextPath).wstring());
        listFolder(std::filesystem::path(path).parent_path().wstring());
        return true;
    }
    void chooseOpen() {
        wchar_t buffer[32768]{};
        OPENFILENAMEW dialog{}; dialog.lStructSize=sizeof(dialog);
        dialog.hwndOwner=hwnd;
        dialog.lpstrFilter=L"Markdown / 文本\0*.md;*.markdown;*.mdown;*.txt\0所有文件\0*.*\0";
        dialog.lpstrFile=buffer; dialog.nMaxFile=32768;
        dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
        if (GetOpenFileNameW(&dialog)) openPath(buffer);
    }
    bool save(bool as) {
        std::wstring target=path;
        const bool choosing=as || path.empty();
        md::FileStamp targetStamp=stamp;
        if (choosing) {
            wchar_t buffer[32768]{};
            if (!path.empty()) wcsncpy(buffer,path.c_str(),32767);
            else wcscpy(buffer,L"未命名.md");
            OPENFILENAMEW dialog{}; dialog.lStructSize=sizeof(dialog);
            dialog.hwndOwner=hwnd; dialog.lpstrFilter=L"Markdown\0*.md\0所有文件\0*.*\0";
            dialog.lpstrDefExt=L"md"; dialog.lpstrFile=buffer; dialog.nMaxFile=32768;
            dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
            if (!GetSaveFileNameW(&dialog)) return false;
            target=buffer;
            std::error_code equivalentError;
            const bool sameFile=!path.empty() && (std::filesystem::equivalent(path,target,equivalentError) ||
                _wcsicmp(std::filesystem::absolute(path).lexically_normal().c_str(),
                         std::filesystem::absolute(target).lexically_normal().c_str())==0);
            if (!sameFile) targetStamp=md::fileStamp(target);
        }
        std::wstring problem;
        md::FileStamp saved;
        if (!syncSource()) return false;
        const auto text=textPrefix();
        if (!md::saveDocument(target,text,encoding,&targetStamp,saved,problem)) { error(problem); return false; }
        path=std::move(target); stamp=saved;
        sci(SCI_SETSAVEPOINT);
        SendMessageW(preview,EM_SETMODIFY,FALSE,0);
        updateTitle(); flash(L"已保存");
        if (!folderPath.empty()) listFolder(folderPath);
        return true;
    }
    void updateTitle() {
        const auto filename=path.empty()?L"未命名.md":std::filesystem::path(path).filename().wstring();
        const auto title=(dirty()?L"● ":L"")+std::wstring(filename)+L" — 轻墨原生版";
        SetWindowTextW(hwnd,title.c_str());
        invalidateHeader(); invalidateStatus();
    }
    void showFind(bool show) {
        findShown=show; layout();
        if (show) { SetFocus(search); SendMessageW(search,EM_SETSEL,0,-1); }
        else SetFocus(activeEditor());
    }
    bool find(bool previous=false, bool fromCurrent=false) {
        const auto query=windowText(search);
        if (query.empty()) { findResult.clear(); invalidateHeader(); return false; }
        bool found=false;
        if (mode==Mode::Source) {
            const auto needle=utf8(query); const auto length=sci(SCI_GETLENGTH);
            const auto start=sci(previous||fromCurrent?SCI_GETSELECTIONSTART:SCI_GETSELECTIONEND);
            sci(SCI_SETSEARCHFLAGS,SCFIND_NONE); sci(SCI_SETTARGETRANGE,start,previous?0:length);
            auto match=sci(SCI_SEARCHINTARGET,needle.size(),reinterpret_cast<sptr_t>(needle.data()));
            if (match<0) { sci(SCI_SETTARGETRANGE,previous?length:0,previous?0:length); match=sci(SCI_SEARCHINTARGET,needle.size(),reinterpret_cast<sptr_t>(needle.data())); }
            if (match>=0) { sci(SCI_SETSEL,sci(SCI_GETTARGETSTART),sci(SCI_GETTARGETEND)); sci(SCI_SCROLLCARET); found=true; }
        } else {
            CHARRANGE selection{}; SendMessageW(preview,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
            FINDTEXTEXW text{{previous?selection.cpMin:(fromCurrent?selection.cpMin:selection.cpMax),previous?0:-1},query.c_str(),{}};
            LRESULT at=SendMessageW(preview,EM_FINDTEXTEXW,previous?0:FR_DOWN,reinterpret_cast<LPARAM>(&text));
            if (at<0) { text.chrg={previous?GetWindowTextLengthW(preview):0,previous?0:-1}; at=SendMessageW(preview,EM_FINDTEXTEXW,previous?0:FR_DOWN,reinterpret_cast<LPARAM>(&text)); }
            if (at>=0) {
                SendMessageW(preview,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&text.chrgText));
                SendMessageW(preview,EM_HIDESELECTION,FALSE,0); SendMessageW(preview,EM_SCROLLCARET,0,0); found=true;
            }
        }
        findResult=found?L"已定位":L"未找到"; invalidateHeader(); return found;
    }
    void format(const char* delimiter) {
        if (GetFocus()!=editor) return;
        const size_t n=strlen(delimiter);
        const auto begin=sci(SCI_GETSELECTIONSTART), end=sci(SCI_GETSELECTIONEND);
        sci(SCI_BEGINUNDOACTION);
        sci(SCI_INSERTTEXT,end,reinterpret_cast<sptr_t>(delimiter));
        sci(SCI_INSERTTEXT,begin,reinterpret_cast<sptr_t>(delimiter));
        sci(SCI_SETSEL,begin+n,end+n);
        sci(SCI_ENDUNDOACTION);
    }
    void synchronizeScroll() {
        if (!syncScroll || !previewShown || dragging) return;
        const auto count=sci(SCI_GETLINECOUNT);
        const auto visible=sci(SCI_LINESONSCREEN);
        const auto first=sci(SCI_DOCLINEFROMVISIBLE,sci(SCI_GETFIRSTVISIBLELINE));
        SCROLLINFO info{}; info.cbSize=sizeof(info); info.fMask=SIF_RANGE|SIF_PAGE;
        if (!GetScrollInfo(preview,SB_VERT,&info)) return;
        const double fraction=count>visible?std::clamp(double(first)/double(count-visible),0.0,1.0):0;
        POINT point{0,static_cast<LONG>(fraction*std::max(0,info.nMax-int(info.nPage)))};
        SendMessageW(preview,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&point));
    }
    void menu() {
        HMENU popup=CreatePopupMenu();
        AppendMenuW(popup,MF_STRING,New,L"新建\tCtrl+N");
        AppendMenuW(popup,MF_STRING,SaveAs,L"另存为…\tCtrl+Shift+S");
        AppendMenuW(popup,MF_STRING,Find,L"查找\tCtrl+F");
        AppendMenuW(popup,MF_STRING,OpenFolder,L"打开文件夹…");
        AppendMenuW(popup,MF_STRING|(focusMode?MF_CHECKED:0),FocusMode,L"全屏专注\tF11");
        AppendMenuW(popup,MF_STRING,About,L"关于原生版");
        RECT rect{}; GetWindowRect(toolbar.back(),&rect);
        int selected=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_RIGHTALIGN,rect.right,rect.bottom,0,hwnd,nullptr);
        DestroyMenu(popup); if (selected) command(selected);
    }
    void command(int id) {
        if (id>=BlockBase && id<=BlockBase+10) {
            if (mode==Mode::Rich) { md::native::setBlock(preview,id-BlockBase); richDirty=true; updateTitle(); SetFocus(preview); }
            else flash(L"段落格式请在可编辑的正文模式中使用。");
            return;
        }
        switch (id) {
        case New: if (confirmDiscard()) setDocument({},{}); break;
        case Open: chooseOpen(); break;
        case Save: save(false); break;
        case SaveAs: save(true); break;
        case Find: showFind(true); break;
        case CloseFind: showFind(false); break;
        case FindNext: find(); break;
        case FindPrevious: find(true); break;
        case EditMode: setMode(Mode::Rich); break;
        case SourceMode: setMode(Mode::Source); break;
        case ReadMode: setMode(mode==Mode::Read?previousMode:Mode::Read); break;
        case TogglePreview: setMode(mode==Mode::Source?Mode::Rich:Mode::Source); break;
        case Refresh: queuePreview(); break;
        case ToggleWrap: wrap=sci(SCI_GETWRAPMODE)==SC_WRAP_NONE; sci(SCI_SETWRAPMODE,wrap?SC_WRAP_CHAR:SC_WRAP_NONE); break;
        case ToggleTheme: dark=!dark; applyTheme(); break;
        case FocusMode: toggleFocus(); break;
        case Bold: case Italic: case Strike: case InlineCode:
            if (mode==Mode::Rich) { md::native::toggleInline(preview,id==Bold?1:id==Italic?2:id==Strike?3:4); richDirty=true; updateTitle(); SetFocus(preview); }
            else if (mode==Mode::Source) { SetFocus(editor); format(id==Bold?"**":id==Italic?"*":id==Strike?"~~":"`"); }
            break;
        case FormatMenu: formattingMenu(); break;
        case FontMenu: typographyMenu(); break;
        case FontLarger: zoom=std::min(160,zoom+10); layout(); break;
        case FontSmaller: zoom=std::max(80,zoom-10); layout(); break;
        case WidePage: widePage=!widePage; layout(); break;
        case More: menu(); break;
        case ToggleFiles: filesShown=!filesShown; layout(); break;
        case OpenFolder: chooseFolder(); break;
        case About:
            MessageBoxW(hwnd,L"轻墨原生版 0.3\nC++ · Windows RichEdit · Scintilla · MD4C\n\n原生所见即所得 / 源码 / 阅读\nCtrl+N / O / S：新建 / 打开 / 保存\nCtrl+B / I：粗体 / 斜体；格式菜单：标题和列表\nCtrl+F：查找；Enter / Shift+Enter：下一处 / 上一处\nF6：源码；F7：阅读；F11：全屏专注\nCtrl+Shift+T：主题；Ctrl + / -：文字缩放\n\n复杂语法保留源码编辑，原生阅读可查看排版。\n无 WebView2、无浏览器后台、不联网。",L"轻墨原生版",MB_OK); break;
        }
        invalidateStatus();
    }
    bool key(const MSG& message) {
        if (message.message!=WM_KEYDOWN && message.message!=WM_SYSKEYDOWN) return false;
        const bool ctrl=GetKeyState(VK_CONTROL)<0, shift=GetKeyState(VK_SHIFT)<0, alt=GetKeyState(VK_MENU)<0;
        const auto key=message.wParam;
        if (key==VK_RETURN && GetFocus()==fileList) { openListedFile(); return true; }
        if (key==VK_RETURN && GetFocus()==search) {
            HIMC context=ImmGetContext(search);
            const bool composing=context && ImmGetCompositionStringW(context,GCS_COMPSTR,nullptr,0)>0;
            if (context) ImmReleaseContext(search,context);
            if (composing) return false;
        }
        int id=0;
        if (ctrl && !alt) {
            if (key=='N') id=New;
            if (key=='O') id=Open;
            if (key=='S') id=shift?SaveAs:Save;
            if (key=='F') id=Find;
            if (key=='B') id=Bold;
            if (key=='I') id=Italic;
            if (key=='T' && shift) id=ToggleTheme;
            if (key=='E' && shift) id=ToggleFiles;
            if (key==VK_OEM_PLUS || key==VK_ADD) id=FontLarger;
            if (key==VK_OEM_MINUS || key==VK_SUBTRACT) id=FontSmaller;
            if (key=='Z' && shift && GetFocus()==preview && mode==Mode::Rich) { SendMessageW(preview,EM_REDO,0,0); return true; }
            if (key==VK_OEM_3) id=InlineCode;
        } else if (!ctrl && !alt) {
            if (key==VK_F3) id=shift?FindPrevious:FindNext;
            if (key==VK_F5) id=Refresh;
            if (key==VK_F6) id=TogglePreview;
            if (key==VK_F7) id=ReadMode;
            if (key==VK_F11) id=FocusMode;
            if (key==VK_ESCAPE && findShown) id=CloseFind;
            if (key==VK_ESCAPE && !findShown && focusMode) id=FocusMode;
            if (key==VK_RETURN && GetFocus()==search) id=shift?FindPrevious:FindNext;
        } else if (!ctrl && alt && key=='Z') id=ToggleWrap;
        if (id) { if (key==VK_F11 && (message.lParam & (1LL<<30))) return true; command(id); return true; }
        return false;
    }
    void persist() {
        if (automated || settingsPath.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(settingsPath).parent_path(),ec);
        if (ec) return;
        for (const auto& item : std::vector<std::pair<const wchar_t*,int>>{{L"dark",dark},{L"wrap",wrap},
            {L"preview",previewShown},{L"sync",syncScroll},{L"split",static_cast<int>(split*100)},{L"zoom",zoom},{L"wide",widePage},{L"files",filesShown},{L"filesWidth",filesWidth}}) {
            WritePrivateProfileStringW(L"view",item.first,std::to_wstring(item.second).c_str(),settingsPath.c_str());
        }
        WritePrivateProfileStringW(L"view",L"folder",folderPath.c_str(),settingsPath.c_str());
    }
    void paint() {
        PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd,&ps); const auto p=colors(dark);
        HBRUSH bg=CreateSolidBrush(p.bg); FillRect(dc,&ps.rcPaint,bg); DeleteObject(bg); SetBkMode(dc,TRANSPARENT);
        auto label=[&](const std::wstring& text,RECT rect,COLORREF color) { auto old=SelectObject(dc,smallFont); SetTextColor(dc,color); DrawTextW(dc,text.c_str(),-1,&rect,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX); SelectObject(dc,old); };
        if (findShown) label(findResult,{client.right-px(320),focusMode?0:px(44),client.right-px(240),editorTop},p.accent);
        if (!focusMode) {
            const auto labelMode=mode==Mode::Rich?L"所见即所得":mode==Mode::Source?L"源码":L"阅读";
            std::wstring status=notice.empty()?(dirty()?L"● 未保存":L"所有更改已保存"):notice;
            if (!richEditable && notice.empty()) status=richReason+L"  ·  源码可编辑";
            label(status,{px(18),contentBottom,client.right-px(200),client.bottom},p.muted);
            label(std::wstring(labelMode)+L"  ·  "+std::to_wstring(zoom)+L"%  ·  "+byteLabel(sci(SCI_GETLENGTH)),{client.right-px(192),contentBottom,client.right-px(12),client.bottom},p.muted);
        }
        EndPaint(hwnd,&ps);
    }
    void drawButton(const DRAWITEMSTRUCT& item) {
        const auto p=colors(dark);
        const bool selected=(item.itemState&ODS_SELECTED)!=0 ||
            (item.CtlID==EditMode && mode==Mode::Rich) || (item.CtlID==SourceMode && mode==Mode::Source) || (item.CtlID==ReadMode && mode==Mode::Read);
        HBRUSH brush=CreateSolidBrush(selected?p.hover:p.bg);
        FillRect(item.hDC,&item.rcItem,brush); DeleteObject(brush);
        auto old=SelectObject(item.hDC,uiFont);
        SetBkMode(item.hDC,TRANSPARENT);
        SetTextColor(item.hDC,item.CtlID==Save?p.accent:p.ink);
        auto text=windowText(item.hwndItem); RECT rect=item.rcItem;
        DrawTextW(item.hDC,text.c_str(),-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if (item.itemState&ODS_FOCUS) { InflateRect(&rect,-px(4),-px(4)); DrawFocusRect(item.hDC,&rect); }
        SelectObject(item.hDC,old);
    }
    static LRESULT CALLBACK richProc(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto* app=reinterpret_cast<App*>(data);
        if (message==WM_CHAR && w==L'\r' && app->mode==Mode::Rich) {
            if (md::native::insertCodeLineBreak(window)) return 0;
        }
        if (message==WM_PASTE && app->mode==Mode::Rich) {
            if (OpenClipboard(window)) {
                HANDLE memory=GetClipboardData(CF_UNICODETEXT);
                if (memory) { const auto* text=static_cast<const wchar_t*>(GlobalLock(memory)); if (text) { SendMessageW(window,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(text)); GlobalUnlock(memory); } }
                CloseClipboard();
            }
            return 0;
        }
        if (message==WM_MOUSEWHEEL && !(GET_KEYSTATE_WPARAM(w)&(MK_CONTROL|MK_SHIFT))) {
            BOOL animate=TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animate,0);
            if (!animate) return DefSubclassProc(window,message,w,l);
            UINT lines=3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
            if (!lines) return 0;
            SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE}; GetScrollInfo(window,SB_VERT,&info);
            POINT point{}; SendMessageW(window,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&point));
            if (!app->scrollMotion.active()) app->scrollMotion.reset(point.y);
            const double delta=-double(GET_WHEEL_DELTA_WPARAM(w))/WHEEL_DELTA*(lines==WHEEL_PAGESCROLL?info.nPage:app->px(24)*lines);
            app->scrollMotion.scrollBy(delta,0,std::max(0,info.nMax-static_cast<int>(info.nPage)+1),static_cast<double>(GetTickCount64()));
            SetTimer(window,20,15,nullptr); return 0;
        }
        if (message==WM_TIMER && w==20) {
            POINT point{0,static_cast<LONG>(app->scrollMotion.sample(static_cast<double>(GetTickCount64())))};
            SendMessageW(window,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&point));
            if (!app->scrollMotion.active()) KillTimer(window,20);
            return 0;
        }
        if (message==WM_KEYDOWN || message==WM_LBUTTONDOWN || message==WM_VSCROLL || message==WM_SIZE) {
            KillTimer(window,20); POINT point{}; SendMessageW(window,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&point)); app->scrollMotion.reset(point.y);
        }
        return DefSubclassProc(window,message,w,l);
    }
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
        App* app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (message==WM_NCCREATE) {
            app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            app->hwnd=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window,message,w,l);
        switch (message) {
        case WM_SIZE:
            app->version.fetch_add(1);
            app->layout();

            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={app->px(640),app->px(360)}; return 0;
        case WM_DPICHANGED:
            app->version.fetch_add(1);
            app->dpi=HIWORD(w); app->updateFonts();
            if (app->editor) SendMessageW(app->editor,message,w,l);
            { auto rect=reinterpret_cast<RECT*>(l); SetWindowPos(window,nullptr,rect->left,rect->top,
                rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE); }
            app->updateMargin(); app->layout(); app->queuePreview(); return 0;
        case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:
            if (app->editor) SendMessageW(app->editor,message,w,l); break;
        case WM_PAINT: app->paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_DRAWITEM: app->drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(w),colors(app->dark).paper);
            SetTextColor(reinterpret_cast<HDC>(w),colors(app->dark).ink);
            return reinterpret_cast<LRESULT>(app->paperBrush);
        case WM_COMMAND:
            if (LOWORD(w)==30) { if (HIWORD(w)==LBN_DBLCLK) app->openListedFile(); return 0; }
            if (LOWORD(w)==11 && HIWORD(w)==EN_CHANGE) {
                if (!app->loading && app->mode==Mode::Rich) { app->richDirty=true; app->updateTitle(); }
                return 0;
            }
            if (LOWORD(w)==20) { if (HIWORD(w)==EN_CHANGE) SetTimer(window,3,160,nullptr); return 0; }
            app->command(LOWORD(w)); return 0;
        case WM_NOTIFY: {
            const auto* notification=reinterpret_cast<SCNotification*>(l);
            if (notification->nmhdr.hwndFrom!=app->editor) break;
            switch (notification->nmhdr.code) {
            case SCN_MODIFIED:
                if (notification->modificationType&(SC_MOD_INSERTTEXT|SC_MOD_DELETETEXT)) {
                    app->changed(); if (!app->loading && notification->linesAdded) app->updateMargin();
                } break;
            case SCN_SAVEPOINTLEFT: case SCN_SAVEPOINTREACHED:
                if (!app->loading) app->updateTitle(); break;
            case SCN_UPDATEUI:
                app->invalidateStatus();
                if (notification->updated&SC_UPDATE_V_SCROLL) app->synchronizeScroll(); break;
            }
            return 0;
        }
        case WM_TIMER:
            if (w==PreviewTimer) { KillTimer(window,PreviewTimer); app->queuePreview(); }
            if (w==3) { KillTimer(window,3); if (app->findShown && !app->largeFile) app->find(false,true); }
            if (w==StatusTimer) { KillTimer(window,StatusTimer); app->notice.clear(); app->invalidateStatus(); }
            return 0;
        case PreviewReady: app->applyPreview(); return 0;
        case WM_LBUTTONDOWN:
            if (app->filesShown && !app->focusMode && abs(GET_X_LPARAM(l)-app->splitX)<=app->px(7) && GET_Y_LPARAM(l)>=app->editorTop) {
                app->dragging=true; SetCapture(window); return 0;
            } break;
        case WM_MOUSEMOVE:
            if (app->dragging && app->client.right) {
                app->version.fetch_add(1);
                app->filesWidth=std::clamp(MulDiv(GET_X_LPARAM(l),96,app->dpi),150,360);
                app->layout(); return 0;
            } break;
        case WM_LBUTTONUP:
            if (app->dragging) { app->dragging=false; ReleaseCapture(); app->queuePreview(); return 0; } break;
        case WM_CAPTURECHANGED: app->dragging=false; break;
        case WM_SETCURSOR:
            if (LOWORD(l)==HTCLIENT) {
                POINT point{}; GetCursorPos(&point); ScreenToClient(window,&point);
                if (app->filesShown && !app->focusMode && abs(point.x-app->splitX)<=app->px(7) && point.y>=app->editorTop) {
                    SetCursor(LoadCursorW(nullptr,IDC_SIZEWE)); return TRUE;
                }
            } break;
        case WM_DROPFILES: {
            HDROP drop=reinterpret_cast<HDROP>(w);
            const UINT length=DragQueryFileW(drop,0,nullptr,0);
            std::wstring file(length+1,L'\0'); DragQueryFileW(drop,0,file.data(),length+1);
            file.resize(length); DragFinish(drop); if (!file.empty()) app->openPath(file); return 0;
        }
        case WM_SETFOCUS: if (app->editor) SetFocus(app->activeEditor()); return 0;
        case WM_CLOSE:
            if (!app->automated && !app->confirmDiscard()) return 0;
            app->persist(); app->shuttingDown=true; app->stop(); DestroyWindow(window); return 0;
        case WM_QUERYENDSESSION: return app->automated || app->confirmDiscard();
        case WM_ENDSESSION: if (w) app->persist(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window,message,w,l);
    }
};

void pump(App& app) {
    MSG message{};
    while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
        if (message.message==WM_QUIT) continue;
        if (!app.key(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
}
bool waitPreview(App& app, DWORD timeout=10000) {
    const auto start=GetTickCount64();
    while (GetTickCount64()-start<timeout) {
        pump(app);
        if (app.displayedVersion==app.version.load()) return true;
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    return false;
}

// Deterministic, hidden integration checks exercise the actual Win32 controls.
int selfTest(App& app) {
    std::vector<std::pair<std::string,bool>> checks;
    auto check=[&](const char* name,bool pass) { checks.emplace_back(name,pass); };
    const std::string original=u8"# 标题 🙂\r\n\r\n**粗体** 与 *斜体*。\r\n\r\n- 第一项\r\n- 第二项\r\n";
    app.setDocument(md::Document{original,md::Encoding::Utf8,{}},L"");
    check("opens_in_native_wysiwyg",app.mode==Mode::Rich && app.richEditable);
    check("unicode_rendered_without_markers",windowText(app.preview).find(L"标题 🙂")!=std::wstring::npos && windowText(app.preview).find(L"**")==std::wstring::npos);
    check("starts_clean",!app.dirty());
    app.setMode(Mode::Read); check("read_only",(GetWindowLongPtrW(app.preview,GWL_STYLE)&ES_READONLY)!=0);
    app.setMode(Mode::Source); check("unchanged_switch_preserves_bytes",app.textPrefix()==original && !app.dirty());
    app.setMode(Mode::Rich);
    CHARRANGE end{-1,-1}; SendMessageW(app.preview,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&end));
    SendMessageW(app.preview,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"新增文字"));
    check("rich_edit_marks_dirty",app.dirty());
    app.setMode(Mode::Read); app.setMode(Mode::Rich);
    check("reading_retains_rich_edit",windowText(app.preview).find(L"新增文字")!=std::wstring::npos);
    SendMessageW(app.preview,EM_UNDO,0,0);
    check("undo_survives_reading",windowText(app.preview).find(L"新增文字")==std::wstring::npos);
    check("undo_retains_original_bytes",app.syncSource() && app.textPrefix()==original);
    app.setDocument(md::Document{u8"第一处文字\n\n第二处文字\n",md::Encoding::Utf8,{}},L"");
    app.setMode(Mode::Read); app.showFind(true); SetWindowTextW(app.search,L"文字");
    check("reading_find",app.find(false,true));
    CHARRANGE first{}; SendMessageW(app.preview,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&first));
    check("reading_find_next",app.find());
    CHARRANGE second{}; SendMessageW(app.preview,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&second));
    check("next_result_moves",second.cpMin>first.cpMin);
    check("find_keeps_input_focus",GetFocus()==app.search);
    check("find_previous",app.find(true));
    SetWindowTextW(app.search,L"没有这个词"); check("find_no_match",!app.find()); app.showFind(false);
    const auto before=app.textPrefix(); app.command(ToggleTheme); app.command(ToggleTheme);
    check("theme_preserves_document",app.textPrefix()==before && !app.dirty());
    ShowWindow(app.hwnd,SW_SHOWNOACTIVATE);
    RECT normal{}; GetWindowRect(app.hwnd,&normal); app.toggleFocus();
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(app.hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
    RECT full{}; GetWindowRect(app.hwnd,&full);
    check("focus_fills_monitor",EqualRect(&full,&monitor.rcMonitor) && !(GetWindowLongPtrW(app.hwnd,GWL_STYLE)&WS_CAPTION));
    app.toggleFocus(); RECT restored{}; GetWindowRect(app.hwnd,&restored);
    check("focus_restores_window",EqualRect(&normal,&restored)); ShowWindow(app.hwnd,SW_HIDE);
    const std::string table="| A | B |\n| --- | --- |\n| 1 | 2 |\n";
    app.setDocument(md::Document{table,md::Encoding::Utf8,{}},L"");
    check("advanced_document_is_readonly",app.mode==Mode::Read && !app.richEditable);
    check("advanced_reading_keeps_contents",windowText(app.preview).find(L"1")!=std::wstring::npos);
    app.setMode(Mode::Rich); check("advanced_edit_uses_source",app.mode==Mode::Source && app.textPrefix()==table);
    app.sci(SCI_APPENDTEXT,1,reinterpret_cast<sptr_t>("x")); check("source_edit_dirty",app.dirty());
    app.sci(SCI_UNDO); check("source_undo_savepoint",!app.dirty() && app.textPrefix()==table);
    std::filesystem::create_directories(L"build/native");
    const auto testPath=std::filesystem::absolute(L"build/native/app-save-test.md").wstring();
    md::FileStamp stamp; std::wstring problem;
    check("fixture_create",md::saveDocument(testPath,original,md::Encoding::Utf16LE,nullptr,stamp,problem));
    app.openPath(testPath,false);
    SendMessageW(app.preview,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&end));
    SendMessageW(app.preview,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"保存修改"));
    check("save_rich_to_markdown",app.save(false));
    md::Document reloaded; bool loaded=md::loadDocument(testPath,reloaded,problem);
    check("saved_file_encoding_and_text",loaded && reloaded.encoding==md::Encoding::Utf16LE && reloaded.text.find(u8"保存修改")!=std::string::npos && reloaded.text.find("\r\n")!=std::string::npos);
    check("saved_state_clean",!app.dirty());
    check("rich_save_keeps_history",SendMessageW(app.preview,EM_CANUNDO,0,0)!=0);
    app.setDocument(md::Document{"```\none\n```\n",md::Encoding::Utf8,{}},L"");
    CHARRANGE codeEnd{3,3}; SendMessageW(app.preview,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&codeEnd));
    SendMessageW(app.preview,WM_CHAR,L'\r',0); SendMessageW(app.preview,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"two"));
    const bool codeSynced=app.syncSource();
    check("code_enter_stays_in_one_block",codeSynced && app.textPrefix()=="```\none\ntwo\n```\n");
    app.setDocument({},{});
    const auto listDir=std::filesystem::absolute(L"build/native/file-list-test");
    std::filesystem::create_directories(listDir/L"子目录");
    md::saveDocument((listDir/L"甲.md").wstring(),"# Alpha\n",md::Encoding::Utf8,nullptr,stamp,problem);
    md::saveDocument((listDir/L"乙.MD").wstring(),"# Beta\n",md::Encoding::Utf8,nullptr,stamp,problem);
    md::saveDocument((listDir/L"ignore.bin").wstring(),"ignored",md::Encoding::Utf8,nullptr,stamp,problem);
    app.listFolder(listDir.wstring());
    check("file_list_filters_and_includes_folders",app.files.size()==4);
    auto entry=std::find_if(app.files.begin(),app.files.end(),[](const auto& item) { return std::filesystem::path(item.path).filename()==L"乙.MD"; });
    check("file_list_case_insensitive_extensions",entry!=app.files.end());
    if (entry!=app.files.end()) { SendMessageW(app.fileList,LB_SETCURSEL,entry-app.files.begin(),0); app.openListedFile(); }
    check("file_list_opens_selected_document",windowText(app.preview).find(L"Beta")!=std::wstring::npos && app.path.find(L"乙.MD")!=std::wstring::npos);
    app.filesShown=true; app.layout(); int beforeWidth=app.splitX;
    app.command(ToggleFiles); check("file_list_collapse_frees_space",beforeWidth>0 && app.splitX==0);
    app.command(ToggleFiles); check("file_list_restore",app.splitX==beforeWidth);
    int failures=0; std::ofstream report("build/native/self-test.json"); report<<"{\n  \"checks\": [\n";
    for(size_t i=0;i<checks.size();++i) { if(!checks[i].second) ++failures; report<<"    {\"name\": \""<<checks[i].first<<"\", \"passed\": "<<(checks[i].second?"true":"false")<<"}"<<(i+1<checks.size()?",":"")<<"\n"; }
    report<<"  ],\n  \"failures\": "<<failures<<"\n}\n"; return failures?1:0;
}

bool captureClient(App& app, const wchar_t* filename) {
    RECT rect{}; GetClientRect(app.hwnd,&rect);
    const int width=rect.right,height=rect.bottom;
    HDC source=GetDC(app.hwnd), memory=CreateCompatibleDC(source);
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(source,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!bitmap) { DeleteDC(memory); ReleaseDC(app.hwnd,source); return false; }
    auto old=SelectObject(memory,bitmap);
    const BOOL captured=PrintWindow(app.hwnd,memory,PW_CLIENTONLY|2);
    const DWORD pixelBytes=static_cast<DWORD>(width*height*4);
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize=header.bfOffBits+pixelBytes;
    HANDLE file=CreateFileW(filename,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    DWORD written=0;
    const bool saved=file!=INVALID_HANDLE_VALUE && WriteFile(file,&header,sizeof(header),&written,nullptr) &&
        WriteFile(file,&info.bmiHeader,sizeof(BITMAPINFOHEADER),&written,nullptr) &&
        WriteFile(file,pixels,pixelBytes,&written,nullptr);
    if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(app.hwnd,source);
    return captured && saved;
}

int snapshot(App& app) {
    std::filesystem::create_directories(L"build/native");
    if (!app.openPath(L"docs\\原生版开始.md",false)) return 1;
    ShowWindow(app.hwnd,SW_SHOWNOACTIVATE); UpdateWindow(app.hwnd); pump(app);
    auto capture=[&](const wchar_t* path) { RedrawWindow(app.hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN); return captureClient(app,path); };
    if (!capture(L"build/native/edit.bmp")) return 2;
    app.setMode(Mode::Read); if (!capture(L"build/native/read.bmp")) return 3;
    app.command(ToggleTheme); if (!capture(L"build/native/dark.bmp")) return 4;
    app.showFind(true); SetWindowTextW(app.search,L"想法"); app.find(false,true);
    if (!capture(L"build/native/find.bmp")) return 5;
    app.showFind(false); app.toggleFocus(); if (!capture(L"build/native/focus.bmp")) return 6; app.toggleFocus();
    app.setMode(Mode::Source); return capture(L"build/native/source.bmp")?0:7;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT com=OleInitialize(nullptr);
    int argc=0; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    const bool test=argc>1 && wcscmp(argv[1],L"--self-test")==0;
    const bool testWindow=argc>1 && wcscmp(argv[1],L"--test-window")==0;
    const bool capture=argc>1 && wcscmp(argv[1],L"--snapshot")==0;

    int result=0;
    try {
        App app(instance,test||capture||testWindow);
        if (!app.initialize()) result=1;
        else if (test) result=selfTest(app);
        else if (capture) result=snapshot(app);
        else {
            if (testWindow && argc>2) app.openPath(argv[2],false);
            else if (!testWindow && argc>1) app.openPath(argv[1],false);
            else {
                wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
                md::Document intro; std::wstring problem;
                if (md::loadDocument((std::filesystem::path(exe).parent_path()/L"原生版开始.md").wstring(),intro,problem)) app.setDocument(std::move(intro),{});
            }
            ShowWindow(app.hwnd,show); UpdateWindow(app.hwnd); SetFocus(app.activeEditor());
            MSG message{};
            while (GetMessageW(&message,nullptr,0,0)>0) {
                if (!app.key(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
        }
    } catch (const std::exception& e) {
        if (!test) MessageBoxW(nullptr,wide(e.what()).c_str(),L"轻墨遇到错误",MB_OK|MB_ICONERROR);
        result=1;
    }
    if (argv) LocalFree(argv);
    if (SUCCEEDED(com)) OleUninitialize();
    return result;
}
