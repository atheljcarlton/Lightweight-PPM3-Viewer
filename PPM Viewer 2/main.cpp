#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cstdint>
#include <algorithm>
#include <windows.h>
#include <commdlg.h> // For GetOpenFileName and OPENFILENAME

// Forward-declare CommandLineToArgvW to avoid pulling in shellapi.h here
extern "C" __declspec(dllimport) LPWSTR* __stdcall CommandLineToArgvW(LPCWSTR lpCmdLine, int* pNumArgs);

// Menu command IDs
constexpr int ID_FILE_OPEN = 9001;

// 1. DATA STRUCTURES
struct Pixel {
    uint8_t b, g, r, a; // Windows expects Blue-Green-Red-Alpha order usually
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels; // Raw memory buffer (The Framebuffer)
};

// Global image variable so the Window Procedure can access it
static Image g_image;

// Helper: adjust window size so client area matches image size
static void SetWindowClientSize(HWND hwnd, int clientWidth, int clientHeight) {
    DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
    RECT rect = {0,0, clientWidth, clientHeight};
    BOOL hasMenu = (GetMenu(hwnd) != NULL);
    AdjustWindowRectEx(&rect, style, hasMenu, exStyle);
    int winW = rect.right - rect.left;
    int winH = rect.bottom - rect.top;
    SetWindowPos(hwnd, NULL, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER);
}

// Helper: convert wide string to UTF-8
static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int size = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string s;
    s.resize(size);
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], size, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// 2. PARSING THE PPM (P3 FORMAT)
// This converts the text "255 0 0" into binary color data in RAM.
Image LoadPPM(const std::string& filepath) {
    Image img;
    std::ifstream file(filepath, std::ios::binary);

    if (!file.is_open()) {
        std::cerr << "Error: Could not open file: " << filepath << std::endl;
        return img;
    }

    // Peek first bytes to detect BOM/encoding
    unsigned char header[4] = {0,0,0,0};
    file.read(reinterpret_cast<char*>(header), 4);
    file.clear();
    file.seekg(0, std::ios::beg);

    bool useStringParser = false;
    std::istringstream ss; // will contain UTF-8 text if needed
    std::string token;

    // Normalizer: strip BOM, UTF-8 NBSP (0xC2 0xA0) and leading ASCII control/space bytes
    auto normalizeToken = [&](std::string &s) {
        bool changed = true;
        while (changed && !s.empty()) {
            changed = false;
            // UTF-8 BOM
            if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF
                && static_cast<unsigned char>(s[1]) == 0xBB
                && static_cast<unsigned char>(s[2]) == 0xBF) {
                s.erase(0, 3);
                changed = true;
                continue;
            }
            // UTF-8 non-breaking space (U+00A0) -> bytes C2 A0
            if (s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xC2
                && static_cast<unsigned char>(s[1]) == 0xA0) {
                s.erase(0, 2);
                changed = true;
                continue;
            }
            // ASCII control or space
            if (static_cast<unsigned char>(s[0]) <= 0x20) {
                s.erase(0, 1);
                changed = true;
                continue;
            }
        }
    };

    // UTF-16 LE BOM
    if (header[0] == 0xFF && header[1] == 0xFE) {
        // Read entire file into memory
        std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        // Convert UTF-16LE bytes (skip BOM) into std::wstring
        std::wstring w;
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            wchar_t wc = static_cast<unsigned char>(raw[i]) | (static_cast<unsigned char>(raw[i+1]) << 8);
            w.push_back(wc);
        }
        // Convert wide string to UTF-8
        if (!w.empty()) {
            int needed = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            if (needed > 0) {
                std::string utf8;
                utf8.resize(needed);
                ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], needed, nullptr, nullptr);
                // build stringstream
                ss.str(utf8);
                useStringParser = true;
            }
        }
    }
    // UTF-16 BE BOM
    else if (header[0] == 0xFE && header[1] == 0xFF) {
        std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::wstring w;
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            wchar_t wc = (static_cast<unsigned char>(raw[i]) << 8) | static_cast<unsigned char>(raw[i+1]);
            w.push_back(wc);
        }
        if (!w.empty()) {
            int needed = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            if (needed > 0) {
                std::string utf8;
                utf8.resize(needed);
                ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], needed, nullptr, nullptr);
                ss.str(utf8);
                useStringParser = true;
            }
        }
    }
    // UTF-8 BOM detection (optional)
    else if (header[0] == 0xEF && header[1] == 0xBB && header[2] == 0xBF) {
        // file is UTF-8 with BOM; skip BOM by reading remaining into string
        std::string rest;
        std::getline(file, rest, '\0');
        // rest currently contains entire file including BOM at start; remove first 3 bytes
        if (rest.size() >= 3) rest = rest.substr(3);
        ss.str(rest);
        useStringParser = true;
    }

    if (useStringParser) {
        // Token reader operating on ss (UTF-8 text)
        auto nextTokenStr = [&](std::string &out) -> bool {
            out.clear();
            while (ss >> out) {
                if (!out.empty() && out[0] == '#') {
                    std::string rest;
                    std::getline(ss, rest);
                    continue;
                }
                normalizeToken(out);
                return true;
            }
            return false;
        };

        if (!nextTokenStr(token)) { std::cerr << "Error: Empty or invalid PPM file." << std::endl; return img; }
        normalizeToken(token);
        if (token != "P3") { std::cerr << "Error: Not a P3 PPM file (expected 'P3')." << std::endl; return img; }

        std::string wtoken, htoken, mtoken;
        if (!nextTokenStr(wtoken)) { std::cerr << "Error: Missing width." << std::endl; return img; }
        if (!nextTokenStr(htoken)) { std::cerr << "Error: Missing height." << std::endl; return img; }
        if (!nextTokenStr(mtoken)) { std::cerr << "Error: Missing maxVal." << std::endl; return img; }

        normalizeToken(wtoken);
        normalizeToken(htoken);
        normalizeToken(mtoken);

        try {
            img.width = std::stoi(wtoken);
            img.height = std::stoi(htoken);
        } catch (...) { std::cerr << "Error: Invalid width/height tokens." << std::endl; return img; }
        int maxVal = 255;
        try { maxVal = std::stoi(mtoken); } catch (...) { std::cerr << "Error: Invalid maxVal token." << std::endl; return img; }

        if (img.width <= 0 || img.height <= 0) { std::cerr << "Error: Invalid image dimensions." << std::endl; return img; }
        const uint64_t pixelCount = static_cast<uint64_t>(img.width) * static_cast<uint64_t>(img.height);
        if (pixelCount == 0 || pixelCount > 100000000) { std::cerr << "Error: Image too large or invalid." << std::endl; return img; }

        img.pixels.resize(static_cast<size_t>(pixelCount));
        for (uint64_t i = 0; i < pixelCount; ++i) {
            std::string sr, sg, sb;
            if (!nextTokenStr(sr) || !nextTokenStr(sg) || !nextTokenStr(sb)) { std::cerr << "Error: Unexpected end of file while reading pixels." << std::endl; img.pixels.clear(); img.width = img.height = 0; return img; }
            normalizeToken(sr); normalizeToken(sg); normalizeToken(sb);
            int r = std::stoi(sr);
            int g = std::stoi(sg);
            int b = std::stoi(sb);
            if (maxVal != 255 && maxVal > 0) {
                r = (r * 255) / maxVal;
                g = (g * 255) / maxVal;
                b = (b * 255) / maxVal;
            }
            r = std::min<int>(255, std::max<int>(0, r));
            g = std::min<int>(255, std::max<int>(0, g));
            b = std::min<int>(255, std::max<int>(0, b));
            uint8_t* ptr = reinterpret_cast<uint8_t*>(&img.pixels[i]);
            ptr[0] = static_cast<uint8_t>(b);
            ptr[1] = static_cast<uint8_t>(g);
            ptr[2] = static_cast<uint8_t>(r);
            ptr[3] = 0;
        }

        std::cout << "P3 Image Loaded: " << img.width << "x" << img.height << std::endl;
        return img;
    }

    // Otherwise proceed with file-stream based parser (existing P3/P6 support)
    // Helper to read the next non-comment token from file stream
    auto nextToken = [&](std::string &out) -> bool {
        out.clear();
        // Skip whitespace and comments
        while (true) {
            int c = file.peek();
            if (c == EOF) return false;
            if (isspace(c)) { file.get(); continue; }
            if (c == '#') {
                std::string rest;
                std::getline(file, rest);
                continue;
            }
            break;
        }
        while (true) {
            int c = file.peek();
            if (c == EOF) break;
            if (isspace(c) || c == '#') break;
            out.push_back(static_cast<char>(file.get()));
        }
        return !out.empty();
    };

    // Rewind file to start
    file.clear(); file.seekg(0, std::ios::beg);

    // Read first token again
    if (!nextToken(token)) { std::cerr << "Error: Empty or invalid PPM file." << std::endl; return img; }

    normalizeToken(token);

    // Strip UTF-8 BOM if present (already handled by normalizeToken, but keep compatibility)
    if (token.size() >= 3 && static_cast<unsigned char>(token[0]) == 0xEF 
        && static_cast<unsigned char>(token[1]) == 0xBB 
        && static_cast<unsigned char>(token[2]) == 0xBF) {
        token = token.substr(3);
    }

    if (token == "P6") {
        // Binary PPM
        std::string wstr, hstr, mstr;
        if (!nextToken(wstr) || !nextToken(hstr) || !nextToken(mstr)) {
            std::cerr << "Error: Malformed P6 header." << std::endl;
            return img;
        }
        normalizeToken(wstr); normalizeToken(hstr); normalizeToken(mstr);
        try {
            img.width = std::stoi(wstr);
            img.height = std::stoi(hstr);
        } catch (...) {
            std::cerr << "Error: Invalid width/height in P6 header: '" << wstr << "' '" << hstr << "'" << std::endl;
            return img;
        }
        int maxVal = 255;
        try { maxVal = std::stoi(mstr); } catch (...) { std::cerr << "Error: Invalid maxVal in P6 header: '" << mstr << "'" << std::endl; return img; }
        if (img.width <= 0 || img.height <= 0) { std::cerr << "Error: Invalid image dimensions." << std::endl; return img; }
        const uint64_t pixelCount = static_cast<uint64_t>(img.width) * static_cast<uint64_t>(img.height);
        if (pixelCount == 0 || pixelCount > 100000000) { std::cerr << "Error: Image too large or invalid." << std::endl; return img; }

        // Consume single whitespace separating header from binary
        int sep = file.get();
        if (sep == EOF) { std::cerr << "Error: Unexpected EOF before pixel data." << std::endl; return img; }
        // allocate
        img.pixels.resize(static_cast<size_t>(pixelCount));
        // Read binary RGB triples
        for (uint64_t i = 0; i < pixelCount; ++i) {
            unsigned char rgb[3];
            file.read(reinterpret_cast<char*>(rgb), 3);
            if (!file) { std::cerr << "Error: Unexpected end of file while reading binary pixels." << std::endl; img.pixels.clear(); img.width = img.height = 0; return img; }
            int r = rgb[0];
            int g = rgb[1];
            int b = rgb[2];
            if (maxVal != 255 && maxVal > 0) {
                r = (r * 255) / maxVal;
                g = (g * 255) / maxVal;
                b = (b * 255) / maxVal;
            }
            uint8_t* ptr = reinterpret_cast<uint8_t*>(&img.pixels[i]);
            ptr[0] = static_cast<uint8_t>(b);
            ptr[1] = static_cast<uint8_t>(g);
            ptr[2] = static_cast<uint8_t>(r);
            ptr[3] = 0;
        }

        std::cout << "P6 Image Loaded: " << img.width << "x" << img.height << std::endl;
        return img;
    }

    if (token != "P3") {
        std::cerr << "Error: Not a P3/P6 PPM file (expected 'P3' or 'P6'). Found: '" << token << "'" << std::endl;
        return img;
    }

    // P3 ASCII parsing
    std::string wtoken, htoken, mtoken;
    if (!nextToken(wtoken)) { std::cerr << "Error: Missing width." << std::endl; return img; }
    if (!nextToken(htoken)) { std::cerr << "Error: Missing height." << std::endl; return img; }
    if (!nextToken(mtoken)) { std::cerr << "Error: Missing maxVal." << std::endl; return img; }

    normalizeToken(wtoken); normalizeToken(htoken); normalizeToken(mtoken);

    try {
        img.width = std::stoi(wtoken);
        img.height = std::stoi(htoken);
    } catch (const std::exception& ex) {
        std::cerr << "Error: Invalid width/height tokens: '" << wtoken << "' '" << htoken << "' - " << ex.what() << std::endl;
        // Dump first bytes for diagnostic
        file.clear(); file.seekg(0, std::ios::beg);
        std::string headerSample;
        headerSample.resize(256);
        file.read(&headerSample[0], 256);
        std::cerr << "File start (first 256 bytes):\n" << headerSample << std::endl;
        return img;
    }
    int maxVal2;
    try { maxVal2 = std::stoi(mtoken); } catch (const std::exception& ex) {
        std::cerr << "Error: Invalid maxVal token: '" << mtoken << "' - " << ex.what() << std::endl;
        return img;
    }

    if (img.width <= 0 || img.height <= 0) {
        std::cerr << "Error: Invalid image dimensions." << std::endl;
        img.width = img.height = 0;
        return img;
    }

    const uint64_t pixelCount2 = static_cast<uint64_t>(img.width) * static_cast<uint64_t>(img.height);
    if (pixelCount2 == 0 || pixelCount2 > 100000000) {
        std::cerr << "Error: Image too large or invalid." << std::endl;
        img.width = img.height = 0;
        return img;
    }

    img.pixels.resize(static_cast<size_t>(pixelCount2));

    // Read the RGB triples (tokens)
    for (uint64_t i = 0; i < pixelCount2; ++i) {
        std::string sr, sg, sb;
        if (!nextToken(sr)) { std::cerr << "Error: Unexpected end of file while reading pixels (r)." << std::endl; img.width = img.height = 0; img.pixels.clear(); return img; }
        if (!nextToken(sg)) { std::cerr << "Error: Unexpected end of file while reading pixels (g)." << std::endl; img.width = img.height = 0; img.pixels.clear(); return img; }
        if (!nextToken(sb)) { std::cerr << "Error: Unexpected end of file while reading pixels (b)." << std::endl; img.width = img.height = 0; img.pixels.clear(); return img; }
        int r,g,b;
        try {
            r = std::stoi(sr);
            g = std::stoi(sg);
            b = std::stoi(sb);
        } catch (const std::exception& ex) {
            std::cerr << "Error: Invalid pixel token at index " << i << ": '" << sr << "' '" << sg << "' '" << sb << "' - " << ex.what() << std::endl;
            // diagnostic dump
            file.clear(); file.seekg(0, std::ios::beg);
            std::string headerSample;
            headerSample.resize(256);
            file.read(&headerSample[0], 256);
            std::cerr << "File start (first 256 bytes):\n" << headerSample << std::endl;
            img.pixels.clear(); img.width = img.height = 0; return img;
        }

        if (maxVal2 != 255 && maxVal2 > 0) {
            r = (r * 255) / maxVal2;
            g = (g * 255) / maxVal2;
            b = (b * 255) / maxVal2;
        }

        r = std::min<int>(255, std::max<int>(0, r));
        g = std::min<int>(255, std::max<int>(0, g));
        b = std::min<int>(255, std::max<int>(0, b));

        uint8_t* ptr = reinterpret_cast<uint8_t*>(&img.pixels[i]);
        ptr[0] = static_cast<uint8_t>(b); // Blue
        ptr[1] = static_cast<uint8_t>(g); // Green
        ptr[2] = static_cast<uint8_t>(r); // Red
        ptr[3] = 0;                        // Padding
    }

    std::cout << "P3 Image Loaded: " << img.width << "x" << img.height << std::endl;
    return img;
}

// 3. THE WINDOW PROCEDURE (The Event Listener)
// This function handles messages from the OS (mouse clicks, resize, "paint now")
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_DESTROY: // User closed the window
        PostQuitMessage(0);
        return 0;

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == ID_FILE_OPEN) {
            OPENFILENAMEW ofn;
            ZeroMemory(&ofn, sizeof(ofn));
            wchar_t szFile[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"PPM Files (*.ppm)\0*.ppm\0All Files\0*.*\0\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameW(&ofn)) {
                std::string path = WideToUtf8(szFile);
                Image img = LoadPPM(path);
                if (img.width > 0 && img.height > 0) {
                    g_image = std::move(img);

                    // Resize window so client area matches image size
                    SetWindowClientSize(hwnd, g_image.width, g_image.height);

                    InvalidateRect(hwnd, NULL, TRUE);
                    UpdateWindow(hwnd);
                } else {
                    MessageBoxW(hwnd, L"Failed to load selected PPM file.", L"Load Error", MB_ICONERROR);
                }
            }
        }
        return 0;
    }

    case WM_PAINT: { // The OS says: "Please draw yourself now"
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (g_image.width > 0 && !g_image.pixels.empty()) {
            // Define how our pixel buffer is formatted
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = g_image.width;
            bmi.bmiHeader.biHeight = -g_image.height; // Negative height tells Windows "Top-Down"
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32; // 32 bits per pixel (B, G, R, Padding)
            bmi.bmiHeader.biCompression = BI_RGB;

            // Copy our RAM buffer to the Window's Video Memory
            StretchDIBits(
                hdc,
                0, 0, g_image.width, g_image.height, // Destination (Window)
                0, 0, g_image.width, g_image.height, // Source (RAM)
                g_image.pixels.data(),               // Pointer to our pixel array
                &bmi,                                // Info about the array
                DIB_RGB_COLORS,
                SRCCOPY
            );
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// 4. MAIN ENTRY POINT
int main(int argc, char** argv) {
    // Optionally load from command line
    if (argc >= 2) {
        g_image = LoadPPM(argv[1]);
    }

    // If no image loaded, create a dummy gradient
    if (g_image.width <= 0 || g_image.height <= 0) {
        g_image.width = 800;
        g_image.height = 600;
        g_image.pixels.resize(g_image.width * g_image.height);
        for (int y = 0; y < g_image.height; ++y) {
            for (int x = 0; x < g_image.width; ++x) {
                int r = (x * 255) / g_image.width;
                int g = (y * 255) / g_image.height;
                int b = 128;

                int index = y * g_image.width + x;
                uint8_t* ptr = reinterpret_cast<uint8_t*>(&g_image.pixels[index]);
                ptr[0] = static_cast<uint8_t>(b);
                ptr[1] = static_cast<uint8_t>(g);
                ptr[2] = static_cast<uint8_t>(r);
                ptr[3] = 0;
            }
        }
    }

    // B. Register the Window Class
    const wchar_t CLASS_NAME[] = L"PPM Viewer Class";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc; // Tell OS which function handles events
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    ATOM atom = RegisterClass(&wc);
    if (!atom) {
        std::cerr << "Error: RegisterClass failed." << std::endl;
        return 1;
    }

    // C. Create the Window
    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"My C++ PPM Viewer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, g_image.width + 16, g_image.height + 39, // Approximate window size
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    if (hwnd == NULL) {
        std::cerr << "Error: CreateWindowEx failed." << std::endl;
        return 1;
    }

    // Create a simple File->Open menu and attach it
    HMENU hMenu = CreateMenu();
    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, ID_FILE_OPEN, L"&Open...");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hFile), L"&File");
    SetMenu(hwnd, hMenu);

    // If an image was loaded from command line, resize window to match it
    if (g_image.width > 0 && g_image.height > 0) {
        SetWindowClientSize(hwnd, g_image.width, g_image.height);
    }

    ShowWindow(hwnd, SW_SHOW);

    // D. The Message Loop (Heartbeat of the app)
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

// D. Provide wWinMain so the app links as a GUI program and still calls main
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    int argc = 0;
    PWSTR* argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argvw) {
        return 0;
    }

    std::vector<std::string> argsStorage;
    argsStorage.reserve(argc);
    std::vector<char*> argv;
    argv.reserve(argc + 1);

    for (int i = 0; i < argc; ++i) {
        std::wstring w = argvw[i];
        std::string s = WideToUtf8(w);
        argsStorage.push_back(std::move(s));
    }
    for (auto &s : argsStorage) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    int result = 0;
    // Call the existing main implementation
    extern int main(int argc, char** argv);
    result = main(argc, argv.data());

    LocalFree(argvw);
    return result;
}