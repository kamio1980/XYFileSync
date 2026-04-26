#define _CRT_SECURE_NO_WARNINGS // 必须放在最顶部

#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <atomic>
#include <filesystem>
#include <map>
#include <chrono>
#include <thread>
#include <mutex>
#include <codecvt> // 用于编码转换
#include <iomanip> // 用于时间格式化
#include <algorithm>
#include <sstream>
#include <ctime>
#include <cwctype>
#include <set>
#include <locale>
#include <unordered_set>
#include <stdexcept>
#include <io.h>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <shlobj.h>  // 必须包含这个头文件才能使用 SHCreateDirectoryExW
#include <utility>  // std::pair
#include <intrin.h>
#include <wbemidl.h>       // 核心 WMI 接口（Win32_BaseBoard 等类在这里）
#include <comdef.h>        // 用于 _bstr_t、_variant_t 等智能包装

// #include <ranges> // vector查询元素用

// 这个是验证SMB凭据时需要用到的
#pragma comment(lib, "Mpr.lib")
#pragma comment(lib, "wbemuuid.lib")  // 链接 WMI UUID 库


using namespace std;
namespace fs = std::filesystem;


// --- 常量与全局变量 ---
const wchar_t* SERVICE_NAME = L"XYFileSync";
const DWORD BUFFER_SIZE = 2097152; // 64KB 缓冲区-65536，1MB缓冲区-1048576
SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;
HANDLE g_hCompPort = INVALID_HANDLE_VALUE; // IOCP 句柄

// DebounceWorker 唤醒事件（Auto-reset）
HANDLE g_DebounceWakeEvent = NULL;

// 关机时配合pre-shutdown使用
std::atomic<bool> g_shutdownFastExit{ false };

struct QueueRow;

// 进程自适应固定时间窗口 + 业务高并发量下减少次数，起到防抖效果，使用此调度程序来适配
enum class EventProcessType {
    MergeQueue,
    ProcessQueue,
    RetrySyncDirPair,
    PushQueue,
    WriteLog,
    WriteError,
    WriteMergeLog,
    WriteFileSyncLog
};

// ===================== 事件调度上下文 =====================
// 每一个业务类型，持有一份自己的调度上下文
struct XYEventSchedulerContext
{
    // ========== 原全局参数，全部下沉到 Context ==========
    std::atomic<bool>     resetFlag{ true };          // 是否允许窗口初始化
    std::atomic<int>      countOtherRequests{ 1 };  // 窗口内额外请求计数
    uint64_t              firstRoundTimeWindow = 500; // 毫秒（首次窗口）
    uint64_t              roundTimeWindow = 1500;     // 毫秒（常规窗口）
    std::atomic<uint64_t> firstTimeInWindow{ 0 };     // 窗口首次时间
    std::atomic<uint64_t> nextTimeInWindow{ 0 };      // 窗口结束时间
    std::atomic<int>      schedulerCounter{ 0 };      // callType=2 计数
};

XYEventSchedulerContext MergeQueueCtx;
XYEventSchedulerContext RetryRegisterDirCtx;
// XYEventSchedulerContext processQueueCtx;
XYEventSchedulerContext PushQueueCtx;
XYEventSchedulerContext WriteLogCtx;
XYEventSchedulerContext WriteErrorCtx;
XYEventSchedulerContext WriteMergeLogCtx;
XYEventSchedulerContext WriteFileSyncLogCtx;

// 缓存所有写文件的行，由调度器控制，减少文件IO
std::vector<std::wstring > g_queueLines;
std::vector<std::wstring > g_writeLogCache;
std::vector<std::wstring > g_writeErrorCache;
std::vector<std::wstring > g_writeMergeLogCache;
std::vector<std::wstring > g_writeFileSyncLogCache;



// 前向声明
struct QueueRow;
std::string ToUTF8(const std::wstring& w);
void RegisterDir(const std::wstring& path);
std::wstring FormattedLine(const std::wstring& line);
void WriteLog(const std::wstring& logMsg, int logLevel);
void WriteError(const std::wstring& errorMsg, const std::wstring& relatedPath, int logLevel);
void PushQueueToFile();
void ProcessQueue(const int& callType);
int GetActionOrder(const std::wstring& action, int matchGroup);
void ProcessQueueRetry(const int& callType);
void XYEventScheduler(XYEventSchedulerContext& ctx, int callType, EventProcessType process);
void LoadCredentialConfig();
void StartUNCSession();
void StopUNCSession();
std::wstring GetFileOrFolderName(const std::wstring& path);
std::wstring GetListenRootPath(const std::wstring& srcFullPath);
void FileOrFolderDeleteSingle(const std::wstring& destFullPath, bool recursive);
std::wstring ToWide(const std::string& str);

// 全局运行标志，启动时加载
bool g_running = true;
bool g_debugModeOnLoad = true;
bool g_debugModeOnPushQueue = false;

// 默认文本分割符
wchar_t g_delimiter = L'"';
// 默认wstring空文本
std::wstring g_emptyWString = L"";




// 全局已知目录集，修正队列中objType不准确问题用
std::set<std::wstring> g_listenAllFolders;
std::mutex g_folderSetMutex;
// 例外目录集，单独清单
std::set<std::wstring> g_listenExcludedFolders;


// --- 目录与文件名配置 ---
std::set<std::wstring> loadGeneralConfigMsg;
std::set<std::wstring> loadGeneralConfigErrorMsg;

std::wstring retryIntervalW = L"2";  // 和chrono::minutes配合使用，因为chrono被业务禁止使用0，需要知道配置参数值时需要转换
std::wstring cfgPath = L"config";
std::wstring logPath = L"log";
std::wstring qPath = L"queue";
std::wstring resPath = L"res";
std::wstring filterCFG = L"ignore-filter.cfg";
std::wstring listenCFG = L"listen-folders.cfg";
std::wstring generalCFG = L"general.cfg";
std::wstring smbCredentialCFG = L"smb-credential.cfg";


std::wstring mergeLOG = L"queue-merge.log";
std::wstring counterRES = L"EventCounter.res";
std::wstring runningCheck = L"service.running";
std::wstring logPrefix = L"log-";
std::wstring errorPrefix = L"err-";
std::wstring logExtension = L".log";
std::wstring listenQ = L"listen.queue";
std::wstring tmpQ = L"temp.queue";
std::wstring fileLOG = L"file-process";
std::wstring mergeSuccessQ = L"2.mergeSuccess.queue";
std::wstring mergeFailedQ = L"2.mergeFailed.queue";
std::wstring mergeRawQ = L"2.mergeRaw.queue";
std::wstring mergeBadQ = L"2.mergeBad.queue";
std::wstring mergeExcludedQ = L"2.mergeEx.queue";

std::wstring fileLOGPrefix = L"filelog-";
std::wstring fileSuccessMiddle = L"success-";
std::wstring fileRetryMiddle = L"retry-";
std::wstring fileFailedMiddle = L"failed-";
std::wstring fileDiscardedMiddle = L"discarded-";

// exclude queue日志不落队列
int disableExcludeQueueLog = 1;
// corrected queue日志不落队列
int disableCorrectQueueLog = 1;
// 禁用两种特殊模式落log
int disableModeCurrentAndFileLog = 1;
// 禁用成功同步日志
int disableFileQueueLog = 1;
// windows延迟加载时长
int serviceDelayStartInSeconds = 5;

// 1 - debug, 2 - info, 3 - warning, 4 - error
int g_logLevel = 3;

// --- 性能参数 ---
int DebounceWaitMilli = 1000;
int DebounceRenameWaitMilli = 1000;
long long initDefaultId = 1000000;
int mergeDelayMilli = 1000;
int mergeRetryMilli = 1500;
int mergeRetryTimes = 5;
// 因子，单位毫秒，队列数量每增加1000条，队列控制MergeQueue的防抖时间就增加这个毫秒数。1000条这个公式写在TryTriggerMergeQueue和MergeQueue代码里
int g_delayOffsetFactor = 1000;
// 毫秒数，在MergeQueue末尾增加额外的数据量偏移，按TryTriggerMergeQueue公式算出来的下一次偏移时间之外额外再加这个偏移送给TryTriggerMergeQueue执行
int g_mergeQueueSelfTriggerOffset = 5000;

// 文件复制选项，1，只覆盖较新；2，强制覆盖；3，跳过已存在
int fileCopyOption = 1;

// 服务启动时是否全量拉平一次
int fullSyncOnLoad = 1;
// 文件复制选项只用于服务启动时，1，只覆盖较新；2，强制覆盖；3，跳过已存在
int fileCopyOptionOnLoad = 3;

// 文件覆盖选项是否移除锁定，1，启用；0，禁用
int fileCopyRemoveLock = 1;

// --- 业务术语配置 (宽字符) ---
std::wstring supportedListenType = L"FOLDER, FILE";
std::wstring syncModeFull = L"full";
std::wstring syncModeCurrent = L"current";
std::wstring syncModeSingleFile = L"FILE"; // 内部处理维度和current类似，但listen目录的配置结构不一样，在两个字段内

std::wstring typeFILE = L"FILE";
std::wstring typeFOLDER = L"FOLDER";
std::wstring actionADD = L"ADDED";
std::wstring actionMOD = L"MODIFIED";
std::wstring actionDEL = L"REMOVED";
std::wstring actionRENOLD = L"RENAME_OLD";
std::wstring actionRENNEW = L"RENAME_NEW";
std::wstring actionUNKNOWN = L"UNKNOWN";
// 一批组合状态，必须要独立出来，否则MergeQueue处理完交给ProcessQueue之后就较难判断了
std::wstring actionADDR = L"ADDED_R";
std::wstring actionDELR = L"REMOVED_R";
std::wstring actionRENOLDR = L"RENAME_OLD_R";
std::wstring actionRENNEWR = L"RENAME_NEW_R";


std::wstring aStatusINIT = L"INIT";
std::wstring aStatusMSUCCESS = L"MPASS";
std::wstring aStatusMBAD = L"MBAD";
std::wstring aStatusMRETRY = L"MRETRY";
std::wstring aStatusMFAILED = L"MFAILED";
std::wstring aStatusMREMOVED = L"MREMOVED";
std::wstring aStatusMEXCLUDED = L"MEXCLUDED";
std::wstring aStatusMCORRECTED = L"MCORRECTED";
std::wstring aStatusMUNKNOWN = L"MUNKNOWN";
std::wstring aStatusMMERGED = L"MMERGED";

std::wstring aStatusPSUCCESS = L"PPASS";
std::wstring aStatusPRETRY = L"PRETRY";
std::wstring aStatusPFAILED = L"PFAILED";
std::wstring aStatusPUNKNOWN = L"PUNKNOWN";


inline uint64_t NowMs()
{
    return GetTickCount64(); // Windows
}




// 定义全局变量，存储程序所在目录
std::wstring g_exeRoot = g_emptyWString;
// 定义初始化函数（在 ServiceMain 或 main 的最开始调用一次）
void InitExeRootPath() {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
        std::wstring strPath = path;
        size_t pos = strPath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            g_exeRoot = strPath.substr(0, pos);
        }
    }
}

enum class CpuVendor {
    Intel,
    AMD,
    Unknown
};

CpuVendor GetCpuVendor()
{
    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 0);

    char vendor[13] = { 0 };
    memcpy(vendor + 0, &cpuInfo[1], 4); // EBX
    memcpy(vendor + 4, &cpuInfo[3], 4); // EDX
    memcpy(vendor + 8, &cpuInfo[2], 4); // ECX

    if (strcmp(vendor, "GenuineIntel") == 0) return CpuVendor::Intel;
    if (strcmp(vendor, "AuthenticAMD") == 0) return CpuVendor::AMD;
    return CpuVendor::Unknown;
}

std::wstring GetCpuBrandString()
{
    int cpuInfo[4] = { 0 };
    char brand[0x40] = { 0 };

    __cpuid(cpuInfo, 0x80000000);
    if ((unsigned)cpuInfo[0] < 0x80000004)
        return L"未知CPU";

    __cpuid((int*)(brand + 0x00), 0x80000002);
    __cpuid((int*)(brand + 0x10), 0x80000003);
    __cpuid((int*)(brand + 0x20), 0x80000004);

    // 去掉首尾多余空格
    std::string s(brand);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();

    return std::wstring(s.begin(), s.end());
}

std::wstring CpuModelString()
{
    switch (GetCpuVendor())
    {
    case CpuVendor::Intel:
    case CpuVendor::AMD:
        return GetCpuBrandString();
    default:
        return L"未知CPU";
    }
}


// 初始化 WMI（公共方法，用于主板和内存查询）
HRESULT InitWMI(IWbemLocator** pLoc, IWbemServices** pSvc) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return hr;

    hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)pLoc);
    if (FAILED(hr)) {
        CoUninitialize();
        return hr;
    }

    hr = (*pLoc)->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, pSvc);
    if (FAILED(hr)) {
        (*pLoc)->Release();
        CoUninitialize();
        return hr;
    }

    hr = CoSetProxyBlanket(*pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr)) {
        (*pSvc)->Release();
        (*pLoc)->Release();
        CoUninitialize();
        return hr;
    }

    return S_OK;
}

// 释放 WMI 资源（公共方法）
void ReleaseWMI(IWbemLocator* pLoc, IWbemServices* pSvc) {
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    CoUninitialize();
}

// 获取主板型号名称（Manufacturer + Product）
std::wstring GetMotherboardModel() {
    IWbemLocator* pLoc = NULL;
    IWbemServices* pSvc = NULL;
    if (FAILED(InitWMI(&pLoc, &pSvc))) return L"未知主板";

    IEnumWbemClassObject* pEnumerator = NULL;
    HRESULT hr = pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT * FROM Win32_BaseBoard"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
    if (FAILED(hr)) {
        ReleaseWMI(pLoc, pSvc);
        return L"未知主板";
    }

    IWbemClassObject* pclsObj = NULL;
    ULONG uReturn = 0;
    std::wstring model = L"未知主板";

    if (SUCCEEDED(pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn)) && uReturn) {
        VARIANT vtProp;
        std::wstring manufacturer, product;

        if (SUCCEEDED(pclsObj->Get(L"Manufacturer", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR) {
            manufacturer = vtProp.bstrVal;
        }
        VariantClear(&vtProp);

        if (SUCCEEDED(pclsObj->Get(L"Product", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR) {
            product = vtProp.bstrVal;
        }
        VariantClear(&vtProp);

        if (!manufacturer.empty() && !product.empty()) {
            // model = manufacturer + L" " + product;
            model = product;
        }

        pclsObj->Release();
    }

    pEnumerator->Release();
    ReleaseWMI(pLoc, pSvc);
    return model;
}

// 内部方法：获取物理内存条信息（容量列表，单位：字节）
std::vector<ULONGLONG> GetPhysicalMemorySticks() {
    std::vector<ULONGLONG> capacities;

    IWbemLocator* pLoc = NULL;
    IWbemServices* pSvc = NULL;
    if (FAILED(InitWMI(&pLoc, &pSvc))) return capacities;

    IEnumWbemClassObject* pEnumerator = NULL;
    HRESULT hr = pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT * FROM Win32_PhysicalMemory"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
    if (FAILED(hr)) {
        ReleaseWMI(pLoc, pSvc);
        return capacities;
    }

    IWbemClassObject* pclsObj = NULL;
    ULONG uReturn = 0;

    while (pEnumerator) {
        hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
        if (uReturn == 0) break;

        VARIANT vtProp;
        if (SUCCEEDED(pclsObj->Get(L"Capacity", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR) {
            ULONGLONG capacity = _wtoi64(vtProp.bstrVal);
            if (capacity > 0) capacities.push_back(capacity);
        }
        VariantClear(&vtProp);
        pclsObj->Release();
    }

    pEnumerator->Release();
    ReleaseWMI(pLoc, pSvc);
    return capacities;
}

// 获取物理内存总容量（GB，四舍五入）
// 参数 useStickFormat: 
//   false → 返回总容量如 "48GB"
//   true  → 返回按条格式如 "24GB * 2"
std::wstring GetPhysicalMemoryTotal(bool useStickFormat) {
    std::vector<ULONGLONG> sticks = GetPhysicalMemorySticks();
    if (sticks.empty()) return L"未知";

    ULONGLONG totalBytes = 0;
    for (auto cap : sticks) totalBytes += cap;

    // 转换为 GB (1024^3)，四舍五入到整数
    double totalGB = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);
    int roundedTotal = static_cast<int>(totalGB + 0.5);  // 四舍五入

    if (!useStickFormat) {
        return std::to_wstring(roundedTotal) + L"GB";
    }

    // ====================== 新增逻辑开始：按容量分组 ======================

    // 将每条内存的容量统一转换为 GB 并四舍五入，用于分组统计
    std::map<int, size_t> capacityGroups; // key: 容量(GB), value: 数量
    for (auto cap : sticks) {
        double gb = static_cast<double>(cap) / (1024.0 * 1024.0 * 1024.0);
        int roundedGB = static_cast<int>(gb + 0.5); // 四舍五入到整数 GB
        capacityGroups[roundedGB]++;
    }
    // 如果所有内存条容量都相同，保持原有打印逻辑（如 16GB * 2）
    if (capacityGroups.size() == 1) {
        int stickGB = capacityGroups.begin()->first;
        size_t numSticks = capacityGroups.begin()->second;

        std::wstringstream ss;
        ss << stickGB << L"GB * " << numSticks;
        return ss.str();
    }
    // 如果存在多种不同容量的内存条，则按容量分组展示
    // 示例：
    //  - 16GB + 32GB
    //  - 16GB * 2 + 32GB * 2
    //  - 16GB * 2 + 32GB + 12GB
    std::wstringstream ss;
    bool first = true;
    for (const auto& [gb, count] : capacityGroups) {
        if (!first) {
            ss << L" + ";
        }
        first = false;
        if (count == 1) {
            ss << gb << L"GB";
        }
        else {
            ss << gb << L"GB * " << count;
        }
    }
    return ss.str();
    // ====================== 新增逻辑结束 ======================
}


// 获取虚拟内存（分页文件）总容量（GB，一位小数，使用 1024 进制）
std::wstring GetVirtualMemoryTotal() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(memInfo);
    if (!GlobalMemoryStatusEx(&memInfo)) return L"未知";

    // 只取分页文件大小（ullTotalPageFile - ullTotalPhys）
    ULONGLONG pageFileBytes = memInfo.ullTotalPageFile - memInfo.ullTotalPhys;
    if (pageFileBytes == 0) return L"0.0GB";

    // 使用 1000 进制转换为 GB
    double pageFileGB = static_cast<double>(pageFileBytes) / (1024.0 * 1024.0 * 1024.0);

    // 四舍五入到一位小数
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1) << pageFileGB;
    return ss.str() + L"GB";
}

// 获取内存代数类型（优先 SMBIOSMemoryType，取第一条，无法辨识 "OTHER"）
// 完整枚举：基于 SMBIOS/JEDEC 标准，补充 GDDR/HBM 等
std::wstring GetMemoryType() {
    IWbemLocator* pLoc = NULL;
    IWbemServices* pSvc = NULL;
    if (FAILED(InitWMI(&pLoc, &pSvc))) return L"OTHER";

    IEnumWbemClassObject* pEnumerator = NULL;
    HRESULT hr = pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT * FROM Win32_PhysicalMemory"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
    if (FAILED(hr)) {
        ReleaseWMI(pLoc, pSvc);
        return L"OTHER";
    }

    IWbemClassObject* pclsObj = NULL;
    ULONG uReturn = 0;
    std::wstring memType = L"OTHER";

    if (SUCCEEDED(pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn)) && uReturn) {
        VARIANT vtProp;
        int typeCode = 0;

        // 优先 SMBIOSMemoryType（DDR4/DDR5 用这个）
        if (SUCCEEDED(pclsObj->Get(L"SMBIOSMemoryType", 0, &vtProp, 0, 0)) && vtProp.vt == VT_I4) {
            typeCode = vtProp.intVal;
        }
        // Fallback MemoryType（旧系统）
        else if (SUCCEEDED(pclsObj->Get(L"MemoryType", 0, &vtProp, 0, 0)) && vtProp.vt == VT_I4) {
            typeCode = vtProp.intVal;
        }
        VariantClear(&vtProp);

        switch (typeCode) {
        case 34: memType = L"DDR5"; break;       // 0x22
        case 35: memType = L"LPDDR5"; break;     // 0x23
        case 27: memType = L"DDR4"; break;       // 0x1B
        case 28: memType = L"LPDDR4"; break;     // 0x1C (或 LPDDR4X)
        case 26: memType = L"DDR3"; break;       // 0x1A
        case 29: memType = L"LPDDR3"; break;     // 0x1D
        case 17: memType = L"DDR2"; break;       // 0x11
        case 13: memType = L"DDR"; break;        // 0x0D
        case 18: memType = L"DDR2 FB-DIMM"; break; // 0x12
        case 36: memType = L"LPDDR2"; break;     // 0x24
        case 37: memType = L"HBM2"; break;       // 0x25 (High Bandwidth Memory)
        case 38: memType = L"HBM3"; break;       // 0x26
        case 32: memType = L"GDDR5"; break;      // 0x20
        case 33: memType = L"GDDR6"; break;      // 0x21
        case 39: memType = L"GDDR7"; break;      // 0x27 (新兴)
        case 30: memType = L"DDR3L"; break;      // 0x1E (低压)
        case 31: memType = L"DDR4L"; break;      // 0x1F
        default: memType = L"OTHER"; break;
        }

        pclsObj->Release();
    }

    pEnumerator->Release();
    ReleaseWMI(pLoc, pSvc);
    return memType;
}






// 为 std::pair<std::wstring, std::wstring> 提供哈希器（用于 unordered_map）
struct PairHash {
    std::size_t operator()(const std::pair<std::wstring, std::wstring>& p) const {
        std::hash<std::wstring> hasher;
        return hasher(p.first) ^ (hasher(p.second) << 1);  // 简单哈希组合
    }
};

// 用于从配置里加载用户凭据，用于注册目录时的校验（无凭据则肯定校验不通过）
struct userCredential {
    std::wstring uncServer;
    std::wstring username;
    std::wstring password;

    bool operator<(const userCredential& other) const {
        if (uncServer != other.uncServer) return uncServer < other.uncServer;
        if (username != other.username) return username < other.username;
        return password < other.password;
    }

    // C++20 三路比较，自动生成 <, >, ==, !=, <=, >=
    // Grok推荐
    // auto operator<=>(const userCredential& other) const = default;

};
std::set<userCredential> g_configuredCredential;

// 由于set原生方法大多数只完整匹配所有字段的组合，无法单独匹配其中一个字段，这里实现一个按 uncServer 统计数量的函数
int countCredentialByUncServer(const std::wstring& inUncServer)
{
    if (inUncServer.empty()) {
        return 0;
    }
    // 创建用于范围查找的边界键（只填充 uncServer，其他字段设为最小值）
    userCredential lowerKey;
    lowerKey.uncServer = inUncServer;

    userCredential upperKey;
    upperKey.uncServer = inUncServer + L"\xFFFF";  // 构造一个大于所有可能后续值的键

    // 获取匹配范围
    auto lower = g_configuredCredential.lower_bound(lowerKey);
    auto upper = g_configuredCredential.upper_bound(upperKey);

    // 计算范围内的元素数量（即 uncServer 完全匹配的记录数）
    return static_cast<int>(std::distance(lower, upper));
}

// 定义 InnerSyncPair 结构体
struct InnerSyncPair {
    std::wstring innerSrcPath;
    std::wstring innerDestPath;
    std::wstring innerSyncMode;
    // ==================== 用于 std::set 排序 ====================
    bool operator<(const InnerSyncPair& other) const {
        if (innerSrcPath != other.innerSrcPath) {
            return innerSrcPath < other.innerSrcPath;
        }
        return innerDestPath < other.innerDestPath;
    }
    // ================================================

    // 可选：添加 operator==，方便调试和判断相等
    bool operator==(const InnerSyncPair& other) const {
        return innerSrcPath == other.innerSrcPath && innerDestPath == other.innerDestPath;
    }
};



// 定义 SyncDir 结构体
struct SyncDir {
    std::wstring listenPath;
    std::wstring destSyncPath;
    std::wstring syncMode;
    std::set<InnerSyncPair> innerPair;
    // ==================== innerPair 的常用方法，套用到SyncDir结构体上 ====================
// 返回 innerPair 中的元素个数
    size_t size() const {
        return innerPair.size();
    }
    // 判断 innerPair 是否为空
    bool empty() const {
        return innerPair.empty();
    }
    // 判断 innerPair 是否有元素（empty 的反义，更直观）
    bool exists() const {
        return !innerPair.empty();
    }
    // 清空 innerPair 中的所有元素
    void clear() {
        innerPair.clear();
    }
    // ================================================
};

// 全局路径对集合
std::vector<SyncDir> g_syncDirPair;
std::vector<SyncDir> g_syncDirPairRetry;

// 操作哪个集合的 ID（可扩展更多集合）
constexpr int SYNC_DIR_PAIR = 1;
constexpr int SYNC_DIR_PAIR_RETRY = 2;


// ==================== 配置的单文件和current模式的配置清单，队列/文件处理和FileOrFolderCopyW使用 ====================
std::vector<SyncDir> syncDirPairHavingSingleFile;
std::vector<SyncDir> syncDirPairHavingCurrent;



// 事件结构体
struct FileEvent {
    std::wstring path;
    std::wstring action;
    std::chrono::system_clock::time_point lastTime;
    std::wstring eventId; // 新增字段
    std::wstring objectType; // 新增：FILE 或 FOLDER
};

// 监控上下文（每个目录一个）
struct DirContext;
struct DirOverlapped {
    OVERLAPPED ov{};
    DirContext* ctx;
};

struct DirContext {
    HANDLE hDir;
    std::wstring wPath;
    BYTE buffer[BUFFER_SIZE];
    DirOverlapped io;
};

// 处理office类文档在保存时经过了多次临时重命名操作的问题
struct RenameMatch {
    std::wstring path;
    std::wstring action;
    std::wstring objType;
    std::wstring eventId;
    std::chrono::system_clock::time_point timestamp;
};

// 互斥锁分开使用，互不嵌套
std::map<std::wstring, RenameMatch> g_renamePending;
std::mutex g_renameMutex;

std::mutex g_logMutex;
std::mutex g_configMutex;

// 全局重试间隔
std::chrono::minutes g_retryInterval(2);
// 全局互斥锁，用于线程安全操作集合
std::mutex g_collectionMutex;


// ========== MergeQueue 调度控制 ==========
std::mutex g_mergeTriggerMutex;                 // 保护调度判断
std::atomic<long long> g_lastMergeTrigger{ 0 }; // 上一次触发时间（ms）
std::atomic<bool> g_mergeRunning{ false };      // 是否已有 MergeQueue 在跑
// ========== queue文件并发控制 ==========g_retryInterval
mutex g_queueMutex;





// --- 工具函数 ---


// 将 wstring 转换为 std::chrono::minutes
// 如果转换失败（非数字、空字符串、负数等），返回 false 并保持 original 值不变
bool WStringToMinutes(const std::wstring& str, std::chrono::minutes& outMinutes)
{
    if (str.empty()) {
        return false;
    }

    // 去除首尾空白（可选，防止 " 5 " 这种）
    std::wstring trimmed = str;
    trimmed.erase(0, trimmed.find_first_not_of(L" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(L" \t\r\n") + 1);

    if (trimmed.empty()) {
        return false;
    }

    // 使用 stoll 解析为 long long（支持大数字，防止溢出）
    try {
        long long value = std::stoll(trimmed);

        // 不允许负数
        if (value < 0) {
            return false;
        }

        outMinutes = std::chrono::minutes(value);
        return true;
    }
    catch (const std::invalid_argument&) {
        // 不是有效数字
        return false;
    }
    catch (const std::out_of_range&) {
        // 数字超出 long long 范围
        return false;
    }
}


// 判断单个 wchar_t 是否是合法 UTF-16 字符（不考虑 BOM），允许基本多语言平面字符和合法代理对起始符
inline bool IsValidWChar(wchar_t ch) {
    // 排除控制字符（除 \r \n \t 可选保留）和非法区段
    if (ch == 0x0000) return false;       // 空字符
    if (ch >= 0xD800 && ch <= 0xDFFF) {  // UTF-16 代理对范围
        return false; // 单独出现的高/低代理码是非法
    }
    // 可选：排除其他不可打印控制字符
    if (ch < 0x20 && ch != L'\t' && ch != L'\n' && ch != L'\r') return false;
    // 可选：排除其他不可显示字符范围
    if (ch >= 0xFFFE && ch <= 0xFFFF) return false;

    return true;
}

// SanitizeLine：过滤掉非法字符
inline std::wstring SanitizeLine(const std::wstring& input, wchar_t replaceChar = L'?') {
    std::wstring output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        wchar_t ch = input[i];
        if (IsValidWChar(ch)) {
            output.push_back(ch);
        }
        else {
            // 高可用策略：替换为 '?' 或者直接跳过
            output.push_back(replaceChar);
        }
    }

    return output;
}

std::wstring MinutesToWString(std::chrono::minutes minutes)
{
    return std::to_wstring(minutes.count());
}

// 辅助函数：UTF-8/GBK/ANSI 转 wstring
std::wstring MultiByteToWString(const std::string& str, UINT codePage = CP_UTF8) {
    if (str.empty()) return g_emptyWString;
    int len = MultiByteToWideChar(codePage, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(codePage, 0, str.data(), (int)str.size(), wstr.data(), len);
    return wstr;
}

// 辅助函数：UTF-16 LE/BE 转 wstring
std::wstring UTF16BytesToWString(const std::vector<unsigned char>& buf, bool swapEndian = false) {
    std::wstring result;
    if (buf.size() % 2 != 0) return result;

    result.resize(buf.size() / 2);
    for (size_t i = 0; i < buf.size(); i += 2) {
        wchar_t ch = swapEndian ? (buf[i] << 8 | buf[i + 1]) : (buf[i + 1] << 8 | buf[i]);
        result[i / 2] = ch;
    }
    return result;
}

// 将任意编码的字节流转换为 std::wstring（UTF-16）
static std::wstring BytesToWString(const std::vector<unsigned char>& buffer) {
    if (buffer.empty()) return g_emptyWString;

    // 检测 BOM
    if (buffer.size() >= 3 &&
        buffer[0] == 0xEF && buffer[1] == 0xBB && buffer[2] == 0xBF) {
        // UTF-8 BOM
        std::string utf8str(buffer.begin() + 3, buffer.end());
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8str.c_str(), (int)utf8str.size(), nullptr, 0);
        std::wstring wstr(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8str.c_str(), (int)utf8str.size(), &wstr[0], size_needed);
        return wstr;
    }
    else if (buffer.size() >= 2 &&
        buffer[0] == 0xFF && buffer[1] == 0xFE) {
        // UTF-16 LE BOM
        std::wstring wstr((wchar_t*)(buffer.data() + 2), (buffer.size() - 2) / 2);
        return wstr;
    }
    else if (buffer.size() >= 2 &&
        buffer[0] == 0xFE && buffer[1] == 0xFF) {
        // UTF-16 BE BOM
        std::wstring wstr;
        wstr.reserve((buffer.size() - 2) / 2);
        for (size_t i = 2; i + 1 < buffer.size(); i += 2) {
            wchar_t wc = (buffer[i] << 8) | buffer[i + 1];
            wstr.push_back(wc);
        }
        return wstr;
    }
    else {
        // 没有 BOM，尝试 UTF-8
        std::string utf8str(buffer.begin(), buffer.end());
        int size_needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8str.c_str(), (int)utf8str.size(), nullptr, 0);
        if (size_needed > 0) {
            std::wstring wstr(size_needed, 0);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8str.c_str(), (int)utf8str.size(), &wstr[0], size_needed);
            return wstr;
        }
        else {
            // fallback: 使用系统 ANSI code page（通常是 GBK）
            size_needed = MultiByteToWideChar(CP_ACP, 0, utf8str.c_str(), (int)utf8str.size(), nullptr, 0);
            std::wstring wstr(size_needed, 0);
            MultiByteToWideChar(CP_ACP, 0, utf8str.c_str(), (int)utf8str.size(), &wstr[0], size_needed);
            return wstr;
        }
    }
}


// 去除字符串中的所有换行符，string和wstring两个版本
std::string RemoveLinebreak(const std::string& stringText) {
    std::string s = stringText;
    s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

std::wstring RemoveWLinebreak(const std::wstring& wStringText)
{
    std::wstring s = wStringText;
    s.erase(std::remove(s.begin(), s.end(), L'\n'), s.end());
    s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
    return s;
}




// 去除注释文本（支持 /* */ 块注释 + # 或 // 行注释，引号内内容保护）
std::wstring RemoveRemarkText(const std::wstring& input)
{
    if (input.empty()) return input;
    std::wstring result = input;
    // ==================== 第 2 类规则：去除 /* ... */ 块注释 ====================
    // 循环查找并删除所有 /* ... */（支持嵌套不考虑，简单实现）
    size_t blockStart;
    const std::wstring BLOCK_START = L"/*";
    const std::wstring BLOCK_END = L"*/";
    while ((blockStart = result.find(BLOCK_START)) != std::wstring::npos) {
        size_t blockEnd = result.find(BLOCK_END, blockStart + BLOCK_START.size());
        if (blockEnd == std::wstring::npos) {
            // 未闭合的 /* 到结尾 → 删除到末尾（常见于残缺注释）
            result.erase(blockStart);
            break;
        }
        // 删除从 /* 到 */ 及其内容（含结束标记）
        result.erase(blockStart, blockEnd - blockStart + BLOCK_END.size());
    }
    // ================================================

    // ==================== 第 1 类规则：去除 # 或 // 开头的行注释（引号外） ====================
    std::set<std::wstring> lineCommentStarters = { L"#", L"//" };
    std::wstring finalResult;
    bool inQuotes = false;           // 当前是否在双引号内（受保护）
    size_t quoteCount = 0;           // 引号计数（奇数=进入，偶数=退出）
    for (size_t i = 0; i < result.size(); ++i) {
        wchar_t ch = result[i];
        // 检查是否遇到双引号
        if (ch == L'"') {
            ++quoteCount;
            inQuotes = (quoteCount % 2 == 1);  // 奇数个引号 → 进入引号内
            finalResult += ch;
            continue;
        }
        // 如果当前在引号内 → 原样保留，不检查注释
        if (inQuotes) {
            finalResult += ch;
            continue;
        }
        // 当前不在引号内 → 检查是否遇到行注释起始符
        bool matchedComment = false;
        for (const auto& starter : lineCommentStarters) {
            if (result.substr(i, starter.size()) == starter) {
                // 找到匹配的注释起始符 → 删除从这里到行末（包括起始符）
                // 找到换行或字符串末尾
                size_t lineEnd = result.find_first_of(L"\r\n", i);
                if (lineEnd == std::wstring::npos) {
                    lineEnd = result.size();
                }
                // 不添加注释部分，直接跳过
                i = lineEnd - 1;  // for 循环会 ++i，抵消后正好跳到换行后
                matchedComment = true;
                break;
            }
        }
        if (matchedComment) {
            // 跳过换行符（\r\n 或 \n）
            if (i + 1 < result.size() && result[i + 1] == L'\n') ++i;
            if (i + 1 < result.size() && result[i + 1] == L'\r') ++i;
            continue;
        }
        // 正常字符，添加到结果
        finalResult += ch;
    }
    // ================================================

    // 如果没有任何修改，返回原字符串（优化）
    return finalResult.empty() ? input : finalResult;
}

// 判断路径是否为 Excel 文件（基于常见扩展名）
bool isExcelFile(const std::wstring& fullPath)
{
    if (fullPath.empty()) {
        return false;
    }

    // ==================== 常见 Excel 扩展名清单（小写） ====================
    static const std::set<std::wstring> excelExtensions = {
        L"xls",   // Excel 97-2003 工作簿
        L"xlsx",  // Excel 2007+ 工作簿
        L"xlsm",  // Excel 2007+ 启用宏的工作簿
        L"xltx",  // Excel 模板
        L"xltm",  // Excel 启用宏的模板
        L"xlsb",  // Excel 二进制工作簿
        L"xlam",  // Excel 加载项
        L"xla"    // Excel 97-2003 加载项（旧版）
    };
    // ================================================

    // 提取文件名（含扩展名）
    std::wstring fileName = GetFileOrFolderName(fullPath);
    if (fileName.empty()) {
        return false;
    }

    // 查找最后一个点号，分离扩展名
    size_t dotPos = fileName.find_last_of(L'.');
    if (dotPos == std::wstring::npos || dotPos == fileName.size() - 1) {
        return false;  // 无扩展名或以点结尾
    }

    // 提取扩展名并转为小写
    std::wstring ext = fileName.substr(dotPos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    // 判断是否在 Excel 扩展名集合中
    return excelExtensions.find(ext) != excelExtensions.end();
}

// 判断两个时间戳是否在允许的秒差范围内（绝对值比较）
// 时间戳格式：YYYY-MM-DD HH:MM:SS
// 示例：almostSameTime(L"2026-01-07 23:39:39", L"2026-01-07 23:40:15", 60) → true
bool almostSameTime(const std::wstring& timestamp1, const std::wstring& timestamp2, int diffInSec)
{
    if (timestamp1.empty() || timestamp2.empty() || diffInSec < 0) {
        return false;
    }

    auto parseTimeToSeconds = [](const std::wstring& ts) -> long long {
        std::wistringstream wiss(ts);
        int year, month, day, hour, minute, second;
        wchar_t dash1, dash2, space, colon1, colon2;

        if (!(wiss >> year >> dash1 >> month >> dash2 >> day >> space >>
            hour >> colon1 >> minute >> colon2 >> second)) {
            return -1;  // 解析失败
        }

        // 简单转换为自 1970-01-01 以来的总秒数（忽略闰年/时区，足够用于相对比较）
        // 使用简化算法：年份从 1970 开始，每年按 365 天，闰年额外 +1
        long long days = (year - 1970) * 365LL +
            (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;  // 粗略闰年

        // 每月天数（非闰年）
        static const int monthDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        for (int m = 1; m < month; ++m) {
            days += monthDays[m - 1];
            if (m == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
                days += 1;  // 闰年 2 月 29 天
            }
        }
        days += day - 1;  // 当天前一天

        long long seconds = days * 86400LL +
            hour * 3600LL +
            minute * 60LL +
            second;

        return seconds;
        };

    long long sec1 = parseTimeToSeconds(timestamp1);
    long long sec2 = parseTimeToSeconds(timestamp2);

    if (sec1 == -1 || sec2 == -1) {
        return false;  // 任意格式非法 → 不相近
    }

    long long diff = (sec1 > sec2) ? (sec1 - sec2) : (sec2 - sec1);
    return diff <= static_cast<long long>(diffInSec);
}

// 获取格式化月份
std::string GetCurrentMonth() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d");
    return ss.str();
}
// 获取格式化日期
std::string GetCurrentDate() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d");
    return ss.str();
}

// 获取格式化日期（宽字符串版本）
std::wstring GetCurrentMonthW()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
#if defined(_WIN32) || defined(_WIN64)
    // Windows 下使用线程安全的 localtime_s
    localtime_s(&bt, &in_time_t);
#else
    // Linux/Unix 下使用线程安全的 localtime_r
    localtime_r(&in_time_t, &bt);
#endif
    std::wstringstream ss;
    ss << std::put_time(&bt, L"%Y-%m");
    return ss.str();
}

// 获取格式化日期（宽字符串版本）
std::wstring GetCurrentDateW()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
#if defined(_WIN32) || defined(_WIN64)
    // Windows 下使用线程安全的 localtime_s
    localtime_s(&bt, &in_time_t);
#else
    // Linux/Unix 下使用线程安全的 localtime_r
    localtime_r(&in_time_t, &bt);
#endif
    std::wstringstream ss;
    ss << std::put_time(&bt, L"%Y-%m-%d");
    return ss.str();
}

// 获取格式化时间戳
std::string GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// 获取格式化时间戳（宽字符串版本）
std::wstring GetCurrentTimestampW()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
#if defined(_WIN32) || defined(_WIN64)
    // Windows 下 localtime_s 更安全
    localtime_s(&bt, &in_time_t);
#else
    // Linux/Unix 下使用 localtime_r
    localtime_r(&in_time_t, &bt);
#endif
    std::wstringstream ss;
    ss << std::put_time(&bt, L"%Y-%m-%d %H:%M:%S");
    return ss.str();
}

enum class LogEncoding { UTF8, UTF16 };
LogEncoding g_logEncoding = LogEncoding::UTF8;
std::wstring StringToWString(const std::string& s);


std::string WStringToUTF8(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
        (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
        result.data(), size_needed, nullptr, nullptr);
    return result;
}

std::wstring StringToWString(const std::string& s) {
    if (s.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), result.data(), size_needed);
    return result;
}



// 辅助方法：去除前后空格
std::wstring TrimWString(std::wstring str) {
    if (str.empty()) return str;
    str.erase(0, str.find_first_not_of(L" "));
    str.erase(str.find_last_not_of(L" ") + 1);
    return str;
}

// 路径规整（同时支持 Windows 本地路径 & SMB UNC 路径）
// 1. 先 FormattedLine
// 2. 去首尾空白、换行
// 3. 保留合法根路径语义
// 4. 去掉非根路径的结尾反斜杠
std::wstring FormatPath(std::wstring path)
{
    std::wstring p = FormattedLine(path);
    p = TrimWString(p);
    p = RemoveWLinebreak(p);

    if (p.empty())
        return g_emptyWString;
    // smb路径
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
    {
        size_t thirdSlash = p.find(L'\\', 2);
        if (thirdSlash == p.size() - 1)
        {
            p.pop_back();
            return p;
        }
        if (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
            p.pop_back();

        return p;
    }
    // windows路径
    if (p.size() >= 2 && p[1] == L':')
    {
        if (p.size() == 3 && p[2] == L'\\')
            return p;
        if (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
            p.pop_back();
        return p;
    }
    if (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    return p;
}

// 获取路径的根/主机部分（统一规整格式）
// 入参: 完整路径 (UNC 或本地)
// 输出: 规整后的根路径，SMB 示例: \\134.98.83.1
std::wstring getFormattedRootPath(const std::wstring& fullPath) {
    std::wstring path = FormattedLine(fullPath); // 先规整

    if (path.empty()) return g_emptyWString;

    // 判断是否UNC路径
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        // 找到第3个反斜杠位置
        size_t pos = path.find(L'\\', 2);
        if (pos != std::wstring::npos) {
            // 截取到第3个反斜杠之前
            return path.substr(0, pos);
        }
        return path; // 本身只有 \\host 的情况
    }

    // 对于本地路径，返回驱动器根，
    size_t pos = path.find(L'\\');
    if (pos != std::wstring::npos) {
        return path.substr(0, pos + 1); // 保留 C:
        return path;
    }
    return path; // 其他情况原样返回
}



/*
 * 统一 UTF-8 写文件方法
 * @param relativeDir   相对目录（可为空，表示当前工作目录 / g_exeRoot）
 * @param fileName      文件名（必填）
 * @param lock          可选互斥锁（可为 nullptr）
 * @param lines         待写入的 wstring 容器
 */
 // writeMode = 1覆盖，2追加
template <typename Container>
bool WriteUTF8LinesToFile(const std::wstring& relativePath, std::mutex* lock, const Container& lines, int writeMode)
{
    // ===== 1. 参数校验 =====
    if (relativePath.empty()) {
        WriteError(L"WriteUTF8LinesToFile 失败：fileName 为空", g_emptyWString, 4);
        return false;
    }
    if (writeMode != 1 && writeMode != 2) {
        WriteError(L"WriteUTF8LinesToFile 失败：writeMode 非法", relativePath, 4);
        return false;
    }

    // ===== 2. 构造最终路径 =====
    fs::path p;
    try {

        p = fs::path(g_exeRoot) / relativePath;
    }
    catch (...) {
        WriteError(L"WriteUTF8LinesToFile 失败：构造文件路径异常", relativePath, 4);
        return false;
    }

    // ===== 3. 加锁（确保所有文件IO操作在锁内）=====
    std::unique_lock<std::mutex> guard;
    if (lock) {
        guard = std::unique_lock<std::mutex>(*lock);
    }

    try {
        // ===== 4. 确保目录存在 =====
        if (p.has_parent_path() && !fs::exists(p.parent_path())) {
            fs::create_directories(p.parent_path());
        }

        // ===== 5. 判断追加模式下是否需要补换行 =====
        bool needLeadingCRLF = false;
        if (writeMode == 2 && fs::exists(p)) {
            std::uintmax_t fileSize = fs::file_size(p);
            if (fileSize > 0) {
                // 使用二进制模式打开以准确定位字节
                std::ifstream ifs(p, std::ios::binary);
                if (ifs.is_open()) {
                    ifs.seekg(-1, std::ios::end);
                    char lastChar = 0;
                    if (ifs.get(lastChar) && lastChar != '\n') {
                        needLeadingCRLF = true;
                    }
                    ifs.close();
                }
            }
        }

        // ===== 6. 打开文件写入 =====
        // 直接传入 fs::path p，解决 Windows 下中文路径无法打开的问题
        std::ios::openmode mode = std::ios::out | std::ios::binary;
        if (writeMode == 1) {
            mode |= std::ios::trunc;
        }
        else {
            mode |= std::ios::app;
        }

        std::ofstream ofs(p, mode);
        if (!ofs.is_open()) {
            WriteError(L"WriteUTF8LinesToFile 失败：无法打开文件写入", p.wstring(), 4);
            return false;
        }

        // ===== 7. 构造写入缓冲区 (减少 IO 次数) =====
        std::string buffer;
        // 预分配内存提高效率
        buffer.reserve(lines.size() * 512);

        if (writeMode == 2 && needLeadingCRLF) {
            buffer.append("\r\n");
        }

        for (const auto& line : lines) {
            buffer.append(ToUTF8(line));
            buffer.append("\r\n");
        }

        // 一次性写入
        if (!buffer.empty()) {
            ofs.write(buffer.data(), buffer.size());
        }

        ofs.flush();
        ofs.close();
        return true;
    }
    catch (const std::exception& e) {
        std::wstring errMsg = L"WriteUTF8LinesToFile 写文件发生异常：" + ToWide(e.what());
        WriteError(errMsg, p.wstring(), 4);
        return false;
    }
    catch (...) {
        WriteError(L"WriteUTF8LinesToFile 写文件发生未知异常", p.wstring(), 4);
        return false;
    }
}




// 辅助函数：将 wstring 转换为 UTF-8 string

static std::mutex debugLogMutex;
void WriteDebugLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(debugLogMutex);
    std::ofstream log("log/debug-err.log", std::ios::app);
    if (!log.is_open()) return;
    log << msg << std::endl;
}

std::string ToUTF8(const std::wstring& wstr) {
    try {
        if (wstr.empty()) return "";

        // 先计算缓冲区长度
        int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
        if (len == 0) {
            // 转换失败，打印原始输入 HEX
            std::ostringstream oss;
            oss << "[SafeToUTF8] WideCharToMultiByte failed, input HEX: ";
            for (wchar_t wc : wstr) {
                oss << std::hex << std::setw(4) << std::setfill('0') << (int)wc << " ";
            }
            WriteDebugLog(oss.str());
            return "";
        }

        std::string utf8(len, 0);
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wstr.data(), (int)wstr.size(), utf8.data(), len, nullptr, nullptr);
        return utf8;
    }
    catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "[SafeToUTF8] exception: " << e.what();
        WriteDebugLog(oss.str());
        return "";
    }
    catch (...) {
        WriteDebugLog("[SafeToUTF8] unknown exception");
        return "";
    }
}


// 1. UTF-8 (string) -> UTF-16 (wstring)
std::wstring ToWide(const std::string& str) {
    if (str.empty()) return g_emptyWString;
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}



// ANSI (GB2312/Windows-1252) -> UTF-16
std::wstring AnsiToWide(const std::string& s) {
    if (s.empty()) return g_emptyWString;
    int len = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}


// 日志方法：普通日志（支持级别控制）
// 日志方法：普通日志（汇聚后写文件）
void WriteLogToFile()
{
    if (g_writeLogCache.empty()) return;
    // 相对路径：使用月目录 + 日志文件
    std::wstring monthLogPath = logPath + L"/" + GetCurrentMonthW();
    fs::create_directories(monthLogPath);
    std::wstring relativePath = monthLogPath + L"/" + logPrefix + GetCurrentMonthW() + logExtension;
    // 调用统一写文件方法（追加模式 writeMode=2）
    WriteUTF8LinesToFile(relativePath, &g_logMutex, g_writeLogCache, 2);
    // 写完文件清空一次缓存
    g_writeLogCache.clear();
}
void WriteLog(const std::wstring& msg, int logLevel = 2)  // 默认 info 级别
{
    // 如果级别不够，且不是强制打印（5），直接返回
    if (logLevel < g_logLevel && logLevel != 5) return;
    // 前缀处理
    std::wstring prefix;
    switch (logLevel) {
    case 1: prefix = L"[调试] "; break;
    case 2: prefix = L"[日志] "; break;
    case 3: prefix = L"[警告] "; break;
    case 4: prefix = L"[错误] "; break;
    case 5: prefix = L"[日志*] "; break;
    default: prefix = L"[其它] "; break;
    }
    // 构造完整一行日志
    std::wstring wline = prefix + GetCurrentTimestampW() + L"：" + RemoveWLinebreak(msg);
    if (msg.empty()) wline = L"\r\n";
    // 先写入缓存，由调度器控制写文件策略，500ms一个窗口期
    g_writeLogCache.push_back(wline);
    if (g_ServiceStatus.dwCurrentState != SERVICE_STOP_PENDING) {
        XYEventScheduler(WriteLogCtx, 1, EventProcessType::WriteLog);
    }
    else {
        WriteLogToFile();
    }
}




// 写入错误日志（UTF-8 编码，支持级别控制）
void WriteErrorToFile()
{
    // 如果级别不够，且不是强制打印（5），直接返回
    if (g_writeErrorCache.empty()) return;

    // 相对路径：使用月目录 + 错误日志文件
    std::wstring monthLogPath = logPath + L"/" + GetCurrentMonthW();
    fs::create_directories(monthLogPath);
    std::wstring relativePath = monthLogPath + L"/" + errorPrefix + GetCurrentMonthW() + logExtension;

    // 调用统一写文件方法（追加模式 writeMode=2）
    WriteUTF8LinesToFile(relativePath, &g_logMutex, g_writeErrorCache, 2);
    // 写完文件清空一次缓存
    g_writeErrorCache.clear();
}
void WriteError(const std::wstring& errorMsg, const std::wstring& relatedPath = g_emptyWString, int logLevel = 4)  // 默认 error 级别
{
    // 如果级别不够，且不是强制打印（5），直接返回
    if (logLevel < g_logLevel && logLevel != 5) return;

    // 前缀处理
    std::wstring prefix;
    switch (logLevel) {
    case 1: prefix = L"[调试] "; break;
    case 2: prefix = L"[日志] "; break;
    case 3: prefix = L"[警告] "; break;
    case 4: prefix = L"[错误] "; break;
    case 5: prefix = L"[日志*] "; break;
    default: prefix = L"[其它] "; break;
    }
    // 构造完整一行日志
    std::wstring wline = prefix + GetCurrentTimestampW() + L"：" + RemoveWLinebreak(errorMsg);
    if (!relatedPath.empty()) {
        wline += L"，" + relatedPath;
    }
    if (errorMsg.empty()) wline = L"\r\n";
    // 先写入缓存，由调度器控制写文件策略，500ms一个窗口期
    g_writeErrorCache.push_back(wline);
    if (g_ServiceStatus.dwCurrentState != SERVICE_STOP_PENDING) {
        XYEventScheduler(WriteErrorCtx, 1, EventProcessType::WriteError);
    }
    else {
        WriteErrorToFile();
    }
}




// MergeQueue 打印日志（支持级别控制）
void WriteMergeLogToFile()
{
    // 如果级别不够，且不是强制打印（5），直接返回
    if (g_writeMergeLogCache.empty()) return;

    // 相对路径：使用月目录 + merge 日志文件
    std::wstring monthLogPath = logPath + L"/" + GetCurrentMonthW();
    fs::create_directories(monthLogPath);
    std::wstring relativePath = monthLogPath + L"/" + mergeLOG + GetCurrentMonthW() + logExtension;

    // 调用统一写文件方法（追加模式 writeMode=2）
    WriteUTF8LinesToFile(relativePath, &g_logMutex, g_writeMergeLogCache, 2);
    // 写完文件清空一次缓存
    g_writeMergeLogCache.clear();
}
void WriteMergeLog(int type, const std::wstring& msg, const std::wstring& eventId, int logLevel = 2)  // 默认 info 级别
{
    // 如果级别不够，且不是强制打印（5），直接返回
    if (logLevel < g_logLevel && logLevel != 5) return;

    // 前缀处理
    std::wstring prefix;
    switch (type) {
    case 1: prefix = L"[MERGE成功] "; break;
    case 2: prefix = L"[MERGE重试中] "; break;
    case 3: prefix = L"[MERGE重试失败] "; break;
    case 4: prefix = L"[MERGE文件性错误] "; break;
    case 5: prefix = L"[MERGE严重异常] "; break;
    default: prefix = L"[MERGE预留-其它] "; break;
    }
    prefix += GetCurrentTimestampW() + L"：";

    // 构造完整一行日志
    std::wstring wline = prefix + RemoveWLinebreak(msg) + L"，Event ID：" + eventId;
    if (msg.empty()) wline = L"\r\n";

    // 先写入缓存，由调度器控制写文件策略，500ms一个窗口期
    g_writeMergeLogCache.push_back(wline);
    if (g_ServiceStatus.dwCurrentState != SERVICE_STOP_PENDING) {
        XYEventScheduler(WriteMergeLogCtx, 1, EventProcessType::WriteMergeLog);
    }
    else {
        WriteMergeLogToFile();
    }
}


// 文件队列处理流程
void WriteFileSyncLogToFile()
{
    if (g_writeFileSyncLogCache.empty()) return;
    // fs::create_directories(logPath);
    std::wstring monthLogPath = logPath + L"/" + GetCurrentMonthW();
    fs::create_directories(monthLogPath);
    std::wstring logFilePath = monthLogPath + L"/" + fileLOG + GetCurrentMonthW() + logExtension;

    // 调用统一写文件方法（追加模式 writeMode=2）
    WriteUTF8LinesToFile(logFilePath, &g_logMutex, g_writeFileSyncLogCache, 2);
    // 写完文件清空一次缓存
    g_writeFileSyncLogCache.clear();
}
void WriteFileSyncLog(const std::wstring& wmsg, const std::wstring& srcFullPath, const std::wstring& destFullPath, int opType, int result, int eventLevel = 2)
{
    if (eventLevel < 1 || eventLevel > 4 || opType < 1 || opType > 5 || result < 1 || result > 3) {
        WriteError(L"WriteFileSyncLog调用参数非法", g_emptyWString, 3);
        return;
    }
    if (eventLevel < g_logLevel) return;
    std::wstring levelStr;
    switch (eventLevel) {
    case 1: levelStr = L"调试"; break;
    case 2: levelStr = L"信息"; break;
    case 3: levelStr = L"警告"; break;
    case 4: levelStr = L"错误"; break;
    }
    std::wstring opStr;
    switch (opType) {
    case 1: opStr = L"新增"; break;
    case 2: opStr = L"修改"; break;
    case 3: opStr = L"重命名"; break;
    case 4: opStr = L"删除"; break;
    case 5: opStr = L"其它"; break;
    }
    std::wstring resStr;
    switch (result) {
    case 1: resStr = L"成功"; break;
    case 2: resStr = L"失败"; break;
    case 3: resStr = L"其它"; break;
    }
    std::wstring wline = L"[ " + levelStr + L" ] [ " + opStr + L" ] [ " + resStr + L" ] 日志，" + GetCurrentTimestampW() + L"：" + RemoveWLinebreak(wmsg);
    if (!srcFullPath.empty()) wline += L"，源路径：" + srcFullPath;
    if (!destFullPath.empty()) wline += L"，目标路径：" + destFullPath;

    std::string line = ToUTF8(wline);
    if (wmsg.empty() && srcFullPath.empty() && destFullPath.empty()) line = "\r\n";

    // 先写入缓存，由调度器控制写文件策略，500ms一个窗口期
    g_writeFileSyncLogCache.push_back(wline);
    if (g_ServiceStatus.dwCurrentState != SERVICE_STOP_PENDING) {
        XYEventScheduler(WriteFileSyncLogCtx, 1, EventProcessType::WriteFileSyncLog);
    }
    else {
        WriteFileSyncLogToFile();
    }
}






// 统计子串在字符串中出现的次数（完全字面量匹配）
int CountString(const std::wstring& source, const std::wstring& target) {
    if (target.empty()) {
        // 显式构造 std::string，确保匹配 const std::string&
        std::wstring wmsg = L"CountString 错误：目标子串不能为空";
        WriteError(wmsg, g_emptyWString, 4);
        return 0;
    }

    int count = 0;
    size_t pos = 0;

    try {
        while ((pos = source.find(target, pos)) != std::wstring::npos) {
            count++;
            pos += target.length();
        }
    }
    catch (const std::exception& e) {
        // 改进：使用 std::string 拼接，并确保作为对象传递
        std::wstring wmsg = L"匹配方法 CountString 发生未知异常：" + ToWide(e.what());
        WriteError(wmsg, g_emptyWString, 4);
        return 0;
    }
    catch (...) {

        std::wstring wmsg = L"匹配方法 CountString 发生无法识别的系统错误";
        WriteError(wmsg, g_emptyWString, 4);
        return 0;
    }
    return count;
}

// 统计char出现次数
size_t CountChar(const std::wstring& s, wchar_t c) {
    return std::count(s.begin(), s.end(), c);
}

// 判断 substring 是否是 string 的前缀（从左边开始完整匹配），如果两个string相等，也返回true
// 目录的判断用这个不准确，严禁使用，这个只用于字符串的匹配情况
bool ContainStringOrEqual(const std::wstring& string, const std::wstring& substring) {
    if (substring.empty()) {
        std::wstring wmsg = L"StartsWith 错误：目标子串不能为空";
        WriteError(wmsg, g_emptyWString, 4);
        return false;
    }

    if (string.size() < substring.size()) {
        return false; // string 比 substring 短，必然不匹配
    }
    return string.compare(0, substring.size(), substring) == 0;
}



// 判断 string 是否以 substring 完整开头（前缀匹配），且剩余部分至少包含一个指定字符
// restChars：需要检查的剩余字符集合（默认包含 '\' 和 '/'）
// 返回 true：string 以 substring 开头，且剩余字符串中至少出现一次 restChars 中的字符。这个方法用来判断是不是子目录也不准，弃用，改用下面的ContainSubFolder
bool ContainFullString(const std::wstring& string, const std::wstring& substring, const std::set<wchar_t>& restChars = { L'\\', L'/' })
{
    if (substring.empty()) {
        WriteError(L"ContainFullString 错误：目标子串不能为空", g_emptyWString, 3);
        return false;
    }
    if (string.size() <= substring.size()) {
        return false;  // string 太短或刚好相等（无剩余字符）
    }
    // 第一步：检查是否完整前缀匹配
    if (string.compare(0, substring.size(), substring) != 0) {
        return false;
    }
    // 第二步：检查剩余部分是否包含至少一个 restChars 中的字符
    const std::wstring& remainder = string.substr(substring.size());
    for (const wchar_t ch : remainder) {
        if (restChars.count(ch) > 0) {
            return true;  // 找到一个即可
        }
    }
    return false;  // 剩余部分不含任何指定字符
}

// 已经修改为当string和substring相同时返回false，相同时也返回true的方法移到ContainSubFolderOrEqual
bool ContainSubFolder(const std::wstring& string, const std::wstring& substring, const std::set<wchar_t>& restChars = { L'\\', L'/' })
{
    if (substring.empty() || string.empty()) {
        WriteError(L"ContainSubFolder 错误：目标子串不能为空", g_emptyWString, 3);
        return false;
    }

    // 如果比较的路径不同的话直接返回false，因为这个方法已经改成判定包含关系、相同的话认为是“不包含”
    if (string == substring) {
        return false;
    }

    if (string.size() < substring.size()) {
        return false;  // string 太短或刚好相等（但上面已处理相等情况）
    }
    // 第一步：检查是否完整前缀匹配
    if (string.compare(0, substring.size(), substring) != 0) {
        return false;
    }

    const std::wstring& remainder = string.substr(substring.size());

    // 情况1：剩余长度为0（已在上方 string == substring 处理）
    // 情况2：剩余长度 >0
    if (remainder.empty()) {
        return true;  // 理论上不会走到这里（已被上方处理），但保险
    }
    // 取剩余部分的第一个字符
    wchar_t firstChar = remainder[0];
    // 如果第一个字符是 restChars 中的任意一个（路径分隔符），返回 true
    if (restChars.count(firstChar) > 0) {
        return true;
    }
    // 否则返回 false
    return false;
    // ================================================
}

// 换个ContainSubFolder名称，同样语义
inline bool PathStartsWith(const std::wstring& fullPath, const std::wstring& prefixPath) {
    return ContainSubFolder(fullPath, prefixPath);
}

// 比较是否完整包含substring，且剩余部分的开头是斜杠或反斜杠
bool ContainSubFolderOrEqual(const std::wstring& string, const std::wstring& substring, const std::set<wchar_t>& restChars = { L'\\', L'/' })
{
    if (substring.empty() || string.empty()) {
        WriteError(L"ContainSubFolderOrEqual 错误：目标子串不能为空", g_emptyWString, 3);
        return false;
    }

    // 相同时也true
    if (string == substring) {
        return true;
    }

    if (string.size() < substring.size()) {
        return false;  // string 太短或刚好相等（但上面已处理相等情况）
    }
    // 第一步：检查是否完整前缀匹配
    if (string.compare(0, substring.size(), substring) != 0) {
        return false;
    }

    const std::wstring& remainder = string.substr(substring.size());

    // 情况1：剩余长度为0（已在上方 string == substring 处理）
    // 情况2：剩余长度 >0
    if (remainder.empty()) {
        return true;  // 理论上不会走到这里（已被上方处理），但保险
    }
    // 取剩余部分的第一个字符
    wchar_t firstChar = remainder[0];
    // 如果第一个字符是 restChars 中的任意一个（路径分隔符），返回 true
    if (restChars.count(firstChar) > 0) {
        return true;
    }
    // 否则返回 false
    return false;
    // ================================================
}



// 这个版本是简单的按单字符分隔符拆分。按分割符|举例，处理的格式是这样的：a|b|c
std::vector<std::wstring> SplitByDelimitedField(const std::wstring& input, wchar_t delimiter) {
    std::vector<std::wstring> result;
    std::wstring current;
    for (wchar_t c : input) {
        if (c == delimiter) {
            // ==================== 新增：格式化当前字段 ====================
            if (!current.empty()) {
                result.push_back(FormattedLine(current));
            }
            else {
                result.push_back(g_emptyWString);  // 连续分隔符产生空字段
            }
            // ================================================
            current.clear();
        }
        else {
            current.push_back(c);
        }
    }
    // ==================== 新增：格式化最后一个字段 ====================
    if (!current.empty()) {
        result.push_back(FormattedLine(current));
    }
    else if (!input.empty() && input.back() == delimiter) {
        // 以分隔符结尾，补一个空字段
        result.push_back(g_emptyWString);
    }
    // ================================================
    return result;
}

// 假设返回解析后的元素向量。假如入参送的是|，预期处理的格式是这样的："a"|"b"|"c"
std::vector<std::wstring> SplitByDelimitedFieldValueQuoted(const std::wstring& input, wchar_t delimiter = g_delimiter)  // 默认是双引号
{
    // 1. 校验 delimiter 是否合法（只能是单个字符，且在允许集合中）
    const std::wstring ALLOWED_DELIMITERS = L",;\?/|\"'-";
    if (delimiter == L'\0' ||
        ALLOWED_DELIMITERS.find(delimiter) == std::wstring::npos)
    {
        std::wstring wmsg = L"分隔符非法：" + std::to_wstring(delimiter);
        WriteError(wmsg, g_emptyWString, 4);
        return {};
    }
    std::vector<std::wstring> elements;
    std::wstring current;
    bool inQuotes = false;  // 是否在“字段”内部

    for (wchar_t c : input)
    {
        if (c == delimiter)
        {
            if (inQuotes)
            {
                // ==================== 新增：结束字段，格式化后加入结果 ====================
                elements.push_back(FormattedLine(current));
                // ================================================
                current.clear();
                inQuotes = false;
            }
            else
            {
                // 开始新字段
                inQuotes = true;
            }
        }
        else if (inQuotes)
        {
            // 在字段内部，直接累加字符（包括空格等）
            current.push_back(c);
        }
        // else: 字段外部的字符直接忽略（标准行为）
    }
    // 处理未闭合的字段（根据需求可以选择加入或丢弃，这里选择丢弃，保持严格）
    if (inQuotes && !current.empty())
    {
        // 可选：如果想保留未闭合字段，取消注释下面一行
        // elements.push_back(FormattedLine(current));
    }
    return elements;
}


namespace fs = std::filesystem;

enum class DirCheckResult {
    Exists,
    NotExists,
    AccessDenied,
    NetworkError,
    InvalidPath,
    OtherError
};

DirCheckResult DirectoryExistsDetails(const std::wstring& pathStr)
{
    if (pathStr.empty()) return DirCheckResult::InvalidPath;

    try {
        fs::path path(pathStr);
        if (fs::exists(path) && fs::is_directory(path))
            return DirCheckResult::Exists;

        return DirCheckResult::NotExists;
    }
    catch (const fs::filesystem_error& e) {
        if (e.code() == std::errc::permission_denied)
            return DirCheckResult::AccessDenied;
        if (e.code() == std::errc::host_unreachable ||
            e.code() == std::errc::timed_out ||
            e.code() == std::errc::network_unreachable)
            return DirCheckResult::NetworkError;
        if (e.code() == std::errc::invalid_argument)
            return DirCheckResult::InvalidPath;

        return DirCheckResult::OtherError;
    }
}

/**
 * 严格检查 Windows 路径（本地或 SMB UNC 路径）对应的目录是否存在
 * @param pathStr 路径字符串（支持 L"X:\\folder" 或 L"\\\\server\\share\\folder"）
 * @return true 表示目录存在且是文件夹，false 表示不存在或不是文件夹或发生错误
 * 但是这个版本没法校验SMB的权限问题，只能简单返回false
 */
bool DirectoryExistsNoCredential(const std::wstring& pathStr)
{
    // 1. 基本空检查
    if (pathStr.empty())
        return false;

    try
    {
        fs::path path(pathStr);

        // 2. 使用 exists() + is_directory() 组合判断
        //    exists() 在网络路径超时或权限问题时会抛异常，所以放在 try 中
        if (fs::exists(path) && fs::is_directory(path))
        {
            return true;
        }

        // 3. 对于某些 SMB 共享，exists() 可能返回 false，但实际可访问
        //    可以额外尝试 status() 来区分“不存在” vs “无法访问”
        //    （可选增强判断）
        fs::file_status status = fs::status(path);
        if (fs::status_known(status) && fs::is_directory(status))
        {
            return true;
        }

        return false;
    }
    catch (const fs::filesystem_error& e)
    {
        // 常见异常情况：
        // - 网络超时
        // - 权限拒绝
        // - 路径语法错误
        // - SMB 服务器不可达
        // std::cerr << L"文件系统错误: " << e.what() << L" (code: " << e.code() << L")\n";
        std::wstring wmsg = L"文件系统错误：" + ToWide(e.what());
        WriteError(wmsg, pathStr, 4);
        return false;
    }
    catch (const std::exception& e)
    {
        // 其他异常（如内存等）
        // std::cerr << L"异常: " << e.what() << L"\n";
        std::wstring wmsg = L"系统异常：" + ToWide(e.what());
        WriteError(wmsg, pathStr, 4);
        return false;
    }
}

// 这一版则靠调GetFileAttributesW来猜，用 Windows API 来判断，略微好些，也不用提供SMB凭据
bool DirectoryExistsByWinAPI(const std::wstring& pathStr)
{
    if (pathStr.empty())
        return false;

    // Windows API 对 UNC 和本地路径都更稳定
    DWORD attr = GetFileAttributesW(pathStr.c_str());

    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        DWORD err = GetLastError();

        switch (err)
        {
        case ERROR_ACCESS_DENIED:
            // ★ 核心策略：
            // 路径存在，但当前服务没有访问权限
            // 对于“是否存在”的弱校验，返回 true
            return true;

        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_FILE_NOT_FOUND:
            return false;

        default:
            // 其他错误：网络异常、服务器不可达等
            return false;
        }
    }

    // 属性可取到，进一步确认是目录
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 这一版使用指定的用户凭据检查路径对应的目录是否存在，支持本地和SMB/UNC路径
// 模式1，给注册目录监听用，用目录自身检查；模式2，给文件复制时判断用，此时用给定路径的父目录
bool DirectoryExistsWithCredential(const std::wstring& inputPath, const int& mode) {
    if (inputPath.empty()) return false;

    namespace fs = std::filesystem;

    // 规整路径
    std::wstring path = FormatPath(inputPath);
    // 2给具体的文件、目录操作时预判断用，决定是否要重试。1给目录监听注册时用
    if (mode == 2) {
        fs::path parent = fs::absolute(path).parent_path();
        path = parent.wstring();
    }

    // 本地路径直接检测
    if (path.size() >= 2 && path[1] == L':') {
        try {
            return fs::exists(path) && fs::is_directory(path);
        }
        catch (...) {
            return false;
        }
    }

    // SMB路径处理
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        std::wstring root = getFormattedRootPath(path); // 获取根路径 \\host 或 \\host\share

        // 在全局凭据集合里匹配根路径
        userCredential matchedCred;
        bool found = false;
        for (const auto& cred : g_configuredCredential) {
            if (getFormattedRootPath(cred.uncServer) == root) {
                matchedCred = cred;
                found = true;
                break;
            }
        }
        if (!found) return false; // 根路径未配置凭据

        // 使用凭据尝试连接
        NETRESOURCEW nr{};
        nr.dwType = RESOURCETYPE_DISK;
        nr.lpLocalName = nullptr; // 不映射驱动器
        nr.lpRemoteName = const_cast<LPWSTR>(path.c_str());
        nr.lpProvider = nullptr;

        // 使用 WNetAddConnection2W 发起临时连接
        DWORD result = WNetAddConnection2W(&nr, matchedCred.password.c_str(),
            matchedCred.username.c_str(), 0);
        if (result != NO_ERROR && result != ERROR_ALREADY_ASSIGNED) {
            return false; // 无法访问
        }
        // 检查路径是否存在
        bool exists = false;
        try {
            exists = fs::exists(path) && fs::is_directory(path);
        }
        catch (...) {
            exists = false;
        }
        // 断开临时连接
        WNetCancelConnection2W(path.c_str(), 0, TRUE);
        return exists;
    }
    // 其他未知情况直接返回 false
    return false;
}



// 这一版使用指定的用户凭据检查路径对应的目录是否存在，支持本地和SMB/UNC路径
// 模式1，给注册目录监听用，用目录自身检查；模式2，给文件复制时判断用，此时用给定路径的父目录
// 返回值：0 = 成功存在； -1 = 不存在； -2 = 凭证错误； -3 = 网络超时/断开； 正数 = 系统错误码；-5 = 新增，专门用于判断目录不存在
int DirectoryExistsWithCredential2Single(const std::wstring& inputPath, const int& mode) {
    if (inputPath.empty()) return -5;  // 【新增】目录不存在（空路径）

    namespace fs = std::filesystem;

    // 规整路径
    std::wstring path = FormatPath(inputPath);
    // 2给具体的文件、目录操作时预判断用，决定是否要重试。1给目录监听注册时用
    if (mode == 2) {
        fs::path parent = fs::absolute(path).parent_path();
        path = parent.wstring();
    }

    // 本地路径直接检测
    if (path.size() >= 2 && path[1] == L':') {
        try {
            if (fs::exists(path) && fs::is_directory(path)) {
                return 0;  // 成功
            }
            return -5;  // 【新增】目录不存在（本地路径查询确认不存在或不是目录）
        }
        catch (const fs::filesystem_error& e) {
            int errCode = e.code().value();
            if (errCode == 2 || errCode == 3) {  // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
                return -5;  // 【新增】明确为目录不存在
            }
            else if (errCode == 5) {  // ERROR_ACCESS_DENIED
                return -2;  // 权限问题（视作凭证错）
            }
            else {
                return errCode;  // 其他系统码
            }
        }
        catch (...) {
            return 87;  // 未知异常，fallback 到 ERROR_INVALID_PARAMETER
        }
    }

    // SMB路径处理
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        std::wstring root = getFormattedRootPath(path); // 获取根路径 \\host 或 \\host\share

        // 在全局凭据集合里匹配根路径
        userCredential matchedCred;
        bool found = false;
        for (const auto& cred : g_configuredCredential) {
            if (getFormattedRootPath(cred.uncServer) == root) {
                matchedCred = cred;
                found = true;
                break;
            }
        }
        if (!found) return -2; // 无凭据 → 视作凭证问题

        // 使用凭据尝试连接
        NETRESOURCEW nr{};
        nr.dwType = RESOURCETYPE_DISK;
        nr.lpLocalName = nullptr; // 不映射驱动器
        nr.lpRemoteName = const_cast<LPWSTR>(root.c_str());  // 用 root 连接 share 级
        nr.lpProvider = nullptr;

        // 使用 WNetAddConnection2W 发起临时连接
        DWORD result = WNetAddConnection2W(&nr, matchedCred.password.c_str(),
            matchedCred.username.c_str(), 0);
        if (result != NO_ERROR && result != ERROR_ALREADY_ASSIGNED) {
            if (result == ERROR_LOGON_FAILURE || result == 1326 || result == ERROR_BAD_USERNAME || result == 2202) {
                return -2;  // 凭证错误
            }
            else if (result == ERROR_BAD_NETPATH || result == 53 || result == ERROR_NETWORK_ACCESS_DENIED || result == 67) {
                return -3;  // 网络问题/超时
            }
            else {
                return static_cast<int>(result);  // 其他系统错误码
            }
        }

        // 检查路径是否存在
        bool exists = false;
        try {
            exists = fs::exists(path) && fs::is_directory(path);
        }
        catch (const fs::filesystem_error& e) {
            int errCode = e.code().value();
            if (errCode == 2 || errCode == 3) {
                exists = false;  // 不存在
            }
            else if (errCode == 5) {
                return -2;  // 访问拒绝（凭证/权限）
            }
            else {
                return errCode;  // 其他
            }
        }
        catch (...) {
            return 87;  // 未知异常
        }

        // 断开临时连接
        WNetCancelConnection2W(root.c_str(), 0, TRUE);

        return exists ? 0 : -5;  // 【新增】目录不存在返回专用值 -5
    }

    // 其他未知路径
    return -5;  // 【新增】未知路径格式，统一视为不存在
}



// 这一版使用指定的用户凭据检查路径对应的目录是否存在，支持本地和SMB/UNC路径
// 模式1，给注册目录监听用，用目录自身检查；模式2，给文件复制时判断用，此时用给定路径的父目录
// 返回值：0 = 成功存在； -1 = 不存在； -2 = 凭证错误； -3 = 网络超时/断开； 正数 = 系统错误码；-5 = 新增，专门用于判断目录不存在
// 【改造】：修改为批量版本，结果以大多数文件的查询结果为准，大多数定义：大于75%
// 【新增】：批量路径版本，返回值与 Single 版完全相同
// 规则：
// 1. 以第一个路径的结果作为初始基准值
// 2. 统计所有路径结果
// 3. 如果某个返回值占比 > 75% → 返回该值（大多数原则）
// 4. 否则 → 返回第一个路径的结果
int DirectoryExistsWithCredential2(const std::set<std::wstring>& inputPaths, const int& mode) {
    if (inputPaths.empty()) {
        return -5;  // 空集合视为不存在
    }

    // 统计每个返回码的出现次数
    std::map<int, size_t> resultCount;
    int firstResult = -5;  // 默认值
    bool firstSet = false;

    for (const std::wstring& inputPath : inputPaths) {
        int singleResult = DirectoryExistsWithCredential2Single(inputPath, mode);

        resultCount[singleResult]++;
        if (!firstSet) {
            firstResult = singleResult;
            firstSet = true;
        }
    }

    size_t total = inputPaths.size();
    size_t threshold = static_cast<size_t>(total * 0.75);  // >75%

    // 查找是否有某个结果超过阈值
    for (const auto& pair : resultCount) {
        if (pair.second > threshold) {
            return pair.first;  // 大多数结果一致，返回该值
        }
    }

    // 没有大多数一致 → 返回第一个路径的结果
    return firstResult;
}


// 1. 服务启动时：递归添加监听目录及其所有子目录
void AddFolderTreeToMem(std::wstring parentPath) {
    std::wstring formattedRoot = FormatPath(parentPath);
    if (formattedRoot.empty()) return;

    std::lock_guard<std::mutex> lock(g_folderSetMutex);

    g_listenAllFolders.insert(formattedRoot);

    try {
        // 使用递归迭代器扫描所有子目录
        for (auto& entry : fs::recursive_directory_iterator(formattedRoot, fs::directory_options::skip_permission_denied)) {
            if (fs::is_directory(entry)) {
                g_listenAllFolders.insert(FormatPath(entry.path().wstring()));
            }
        }
    }
    catch (...) {
        // 捕获权限不足等异常
    }
}

// 2. 单个目录添加
// set天然去重，无需额外判断
void AddSingleSubFolderToMem(std::wstring path) {
    std::wstring p = FormatPath(path);
    if (p.empty()) return;
    std::lock_guard<std::mutex> lock(g_folderSetMutex);
    g_listenAllFolders.insert(p);
}

// 3. 单个目录移除
void DeleteSubFolderFromMem(std::wstring path) {
    std::wstring p = FormatPath(path);
    if (p.empty()) return;
    std::lock_guard<std::mutex> lock(g_folderSetMutex);
    g_listenAllFolders.erase(p);
}

// 单独添加一个例外目录（不递归子目录，在文件处理时再判断，这里只维护配置里指定的例外目录，即作为例外的根目录）
// set天然去重，无需额外判断
void AddSingleExcludeFolderToMem(std::wstring path) {
    std::wstring p = FormatPath(path);
    if (p.empty()) return;
    std::lock_guard<std::mutex> lock(g_folderSetMutex);
    g_listenExcludedFolders.insert(p);
}

// 单独添加一个例外目录（不递归子目录）
void DeleteExcludeFolderFromMem(std::wstring path) {
    std::wstring p = FormatPath(path);
    if (p.empty()) return;
    std::lock_guard<std::mutex> lock(g_folderSetMutex);
    g_listenExcludedFolders.erase(p);
}



// 添加到集合，用于维护原目录、目标目录对
void AddToCollection(int collectionId, const std::wstring& listen, const std::wstring& dest, const std::wstring& syncMode, const std::set<InnerSyncPair>& innerSyncPairs) {
    std::wstring formattedListen = FormatPath(listen); // 无论普通还是重试记录，如果模式是单文件，这里是单文件的父目录；模式不是单文件，是注册目录本身
    std::wstring formattedDest = FormatPath(dest);


    // ===== 1. 基本校验：不能为空 =====
    if (formattedListen.empty() || formattedDest.empty()) {
        return; // 无效路径，直接返回
    }

    // ===== 2. 针对正式集合 SYNC_DIR_PAIR 的校验 =====
    if (collectionId == SYNC_DIR_PAIR) {
        bool listenExists = DirectoryExistsWithCredential(formattedListen, 1);
        bool destExists = DirectoryExistsWithCredential(formattedDest, 1);

        if (!listenExists || !destExists) {
            // 任意一方不存在或无法访问，则不加入正式集合
            return;
        }
    }
    // ===== 3. 对于 SYNC_DIR_PAIR_RETRY 不做目录有效性校验 =====

    // ===== 4. 构造条目并加入集合 =====
    std::wstring sm = syncMode.empty() ? syncModeFull : syncMode;
    SyncDir entry{ formattedListen, formattedDest, sm, innerSyncPairs };

    // 同一个监听目录的不同形式的配置，像loadlisten方法一样合并相同目录的记录
    if (collectionId == SYNC_DIR_PAIR_RETRY) {
        auto matchedRetry = std::ranges::find_if(g_syncDirPairRetry, [&](const SyncDir& s) { return s.listenPath == formattedListen; });
        if (matchedRetry != g_syncDirPairRetry.end()) {
            std::wstring smr = matchedRetry->syncMode;
            InnerSyncPair requestPair;
            // 入参传入代表的是原始配置行，虽然是set，实际最多只有一条记录
            if (sm == syncModeSingleFile && !innerSyncPairs.empty()) requestPair = *innerSyncPairs.begin();
            // 如果入参是目录模式，作为多目标要添加时，本行配置转为要添加到内层的记录格式
            if (sm != syncModeSingleFile) {
                requestPair.innerSrcPath = formattedListen;
                requestPair.innerDestPath = formattedDest;
                requestPair.innerSyncMode = sm;
            }

            // 存在记录的外层是单文件模式
            if (smr == syncModeSingleFile) {
                bool dup = false;
                for (const auto& inner : matchedRetry->innerPair) {
                    // 是否重复，区分单文件模式要多比对src是否相同
                    if (sm == syncModeSingleFile && inner.innerSrcPath == requestPair.innerSrcPath && inner.innerDestPath == requestPair.innerDestPath) {
                        dup = true;
                        break;
                    }
                    else if (sm != syncModeSingleFile && inner.innerDestPath == requestPair.innerDestPath) {
                        dup = true;
                        break;
                    }
                }

                if (dup) return; // 任意模式下的目标重复了，都直接跳出不做任何事

                // 当前欲添加记录也是单文件、且未重复，多目标添加当前，外层保持单文件模式不动
                if (sm == syncModeSingleFile && !dup) {
                    matchedRetry->innerPair.insert(requestPair);
                    return;
                }
                // 当前欲添加记录是目录模式、且未重复，内层保持不变（此时内层所有记录都是单文件），外层更新为本次入参目录对，监听目录不用动
                else if (sm != syncModeSingleFile && !dup) {
                    matchedRetry->destSyncPath = formattedDest;
                    matchedRetry->syncMode = sm;
                    return;
                }

            }
            else if (smr != syncModeSingleFile) {
                bool dup = false;
                if (formattedDest == matchedRetry->destSyncPath) dup = true; // 外层判断重复

                for (const auto& inner : matchedRetry->innerPair) {
                    // 是否重复，区分单文件模式要多比对src是否相同
                    if (sm == syncModeSingleFile && inner.innerSrcPath == requestPair.innerSrcPath && inner.innerDestPath == requestPair.innerDestPath) {
                        dup = true;
                        break;
                    }
                    else if (sm != syncModeSingleFile && inner.innerDestPath == requestPair.innerDestPath) {
                        dup = true;
                        break;
                    }
                }

                if (dup)  return;
                // 未重复情况下无论入参配置是什么模式，无论外层是full还是current都直接往内层添加多目标，外层不动
                matchedRetry->innerPair.insert(requestPair);
                return;
            }
        } // 没有else，如果当前没有记录则按入参组装的entry首次添加到retry集合

    }

    std::lock_guard<std::mutex> lock(g_collectionMutex);
    auto& collection = (collectionId == SYNC_DIR_PAIR) ? g_syncDirPair : g_syncDirPairRetry;
    collection.push_back(entry);
}






// 从集合中删除（根据 listenPath 匹配删除）
void RemoveFromCollection(int collectionId, const std::wstring& listen) {
    std::wstring formattedListen = FormatPath(listen);

    std::lock_guard<std::mutex> lock(g_collectionMutex);
    auto& collection = (collectionId == SYNC_DIR_PAIR) ? g_syncDirPair : g_syncDirPairRetry;
    collection.erase(std::remove_if(collection.begin(), collection.end(),
        [&](const SyncDir& item) {
            return FormatPath(item.listenPath) == formattedListen;
        }),
        collection.end());
}

// 获取集合大小
size_t GetCollectionSize(int collectionId) {
    std::lock_guard<std::mutex> lock(g_collectionMutex);
    const auto& collection = (collectionId == SYNC_DIR_PAIR) ? g_syncDirPair : g_syncDirPairRetry;
    return collection.size();
}

// 获取集合（返回拷贝以避免并发问题）
std::vector<SyncDir> GetCollection(int collectionId) {
    std::lock_guard<std::mutex> lock(g_collectionMutex);
    return (collectionId == SYNC_DIR_PAIR) ? g_syncDirPair : g_syncDirPairRetry;
}

// 遍历集合（示例：打印所有条目，实际可替换为 lambda 或其他处理）
void TraverseCollection(int collectionId) {
    std::lock_guard<std::mutex> lock(g_collectionMutex);
    const auto& collection = (collectionId == SYNC_DIR_PAIR) ? g_syncDirPair : g_syncDirPairRetry;
    for (const auto& entry : collection) {
        // 示例：处理 entry.listenPath 和 entry.destSyncPath
        // std::wcout << entry.listenPath << L" -> " << entry.destSyncPath << std::endl;
    }
}

// 判断某对目录是否存在于集合中，两个字段都会判断
bool ExistsInCollection(const std::vector<SyncDir>& collection,
    const std::wstring& listenPath,
    const std::wstring& destSyncPath)
{
    std::wstring formattedListen = FormatPath(listenPath);
    std::wstring formattedDest = FormatPath(destSyncPath);

    if (formattedListen.empty() || formattedDest.empty())
        return false;  // 可选：无效路径视为不存在

    return std::any_of(collection.begin(), collection.end(),
        [&](const SyncDir& item) {
            return FormatPath(item.listenPath) == formattedListen &&
                FormatPath(item.destSyncPath) == formattedDest;
        });
}
// 判断某目标目录是否存在于集合中（仅 目标）
bool ExistsDestInCollection(const std::vector<SyncDir>& collection,
    const std::wstring& listenPath,          // 保留此参数以保持接口一致（可不使用）
    const std::wstring& destSyncPath)
{
    std::wstring formattedListen = FormatPath(listenPath);
    std::wstring formattedDest = FormatPath(destSyncPath);

    if (formattedDest.empty())
        return false;  // 无效路径视为不存在

    return std::any_of(collection.begin(), collection.end(),
        [&](const SyncDir& item) {
            return FormatPath(item.destSyncPath) == formattedDest;
        });
}



// 原来用
std::vector<unsigned char> ReadAllBytes(const std::wstring& filename) {
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs) return {};

    ifs.seekg(0, std::ios::end);
    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<unsigned char> buffer(size);
    if (!buffer.empty()) {
        ifs.read(reinterpret_cast<char*>(buffer.data()), size);
    }
    return buffer;
}

// 文件转为wide string vector
std::vector<std::wstring> FileToWStringLinesXY(const std::wstring& filePath)
{
    std::vector<std::wstring> result;

    // 1. 检查文件存在
    if (!fs::exists(filePath))
        return result;

    // 2. 打开文件
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open())
        return result;

    // 3. 读取整个文件到 std::string
    std::string content((std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());
    ifs.close();

    // 4. 处理 UTF-8 -> std::wstring
#ifdef _WIN32
    // Windows 使用 MultiByteToWideChar
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, content.data(), (int)content.size(), nullptr, 0);
    if (wideLen <= 0)
        throw std::runtime_error("MultiByteToWideChar失败");

    std::wstring wcontent(wideLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, content.data(), (int)content.size(), wcontent.data(), wideLen);

    // 5. 按行拆分
    size_t start = 0;
    while (start < wcontent.size())
    {
        size_t end = wcontent.find(L'\n', start);
        std::wstring line;
        if (end == std::wstring::npos)
        {
            line = wcontent.substr(start);
            start = wcontent.size();
        }
        else
        {
            line = wcontent.substr(start, end - start);
            start = end + 1;
        }

        // 去掉可能的 \r
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();

        result.push_back(line);
    }
#else
    // Linux / Ubuntu
    std::wstring wcontent;
    wcontent.resize(content.size()); // 临时缓冲
    size_t converted = mbstowcs(&wcontent[0], content.c_str(), content.size());
    if (converted == (size_t)-1)
        throw std::runtime_error("mbstowcs failed");

    wcontent.resize(converted);

    // 按行拆分
    size_t start = 0;
    while (start < wcontent.size())
    {
        size_t end = wcontent.find(L'\n', start);
        std::wstring line;
        if (end == std::wstring::npos)
        {
            line = wcontent.substr(start);
            start = wcontent.size();
        }
        else
        {
            line = wcontent.substr(start, end - start);
            start = end + 1;
        }

        // 去掉可能的 \r
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();

        result.push_back(line);
    }
#endif

    return result;
}

// 文件转为wide string set
std::set<std::wstring> FileToWStringSetXY(const std::string& filePath)
{
    std::set<std::wstring> result;

    // 1. 检查文件存在
    if (!fs::exists(filePath))
        return result;

    // 2. 打开文件
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open())
        return result;

    // 3. 读取整个文件到 std::string
    std::string content((std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());
    ifs.close();

    // 4. 处理 UTF-8 -> std::wstring
#ifdef _WIN32
    // Windows 使用 MultiByteToWideChar
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, content.data(), (int)content.size(), nullptr, 0);
    if (wideLen <= 0)
        throw std::runtime_error("MultiByteToWideChar失败");

    std::wstring wcontent(wideLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, content.data(), (int)content.size(), wcontent.data(), wideLen);

    // 5. 按行拆分
    size_t start = 0;
    while (start < wcontent.size())
    {
        size_t end = wcontent.find(L'\n', start);
        std::wstring line;
        if (end == std::wstring::npos)
        {
            line = wcontent.substr(start);
            start = wcontent.size();
        }
        else
        {
            line = wcontent.substr(start, end - start);
            start = end + 1;
        }

        // 去掉可能的 \r
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();

        result.insert(line);
    }
#else
    // Linux / Ubuntu
    std::wstring wcontent;
    wcontent.resize(content.size()); // 临时缓冲
    size_t converted = mbstowcs(&wcontent[0], content.c_str(), content.size());
    if (converted == (size_t)-1)
        throw std::runtime_error("mbstowcs failed");

    wcontent.resize(converted);

    // 按行拆分
    size_t start = 0;
    while (start < wcontent.size())
    {
        size_t end = wcontent.find(L'\n', start);
        std::wstring line;
        if (end == std::wstring::npos)
        {
            line = wcontent.substr(start);
            start = wcontent.size();
        }
        else
        {
            line = wcontent.substr(start, end - start);
            start = end + 1;
        }

        // 去掉可能的 \r
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();

        result.insert(line);
    }
#endif

    return result;
}




// 自定义文件对象类型
struct xyFileObject {
    std::wstring objType;        // L"File" 或 L"Folder"
    std::wstring objName;        // 文件/文件夹名称，含扩展名
    std::wstring objFullPath;    // 完整绝对路径
    std::wstring pathType;       // L"Windows" 或 L"UNC"
    uint64_t fileSize = 0;       // 文件大小（字节），文件夹为0
    std::wstring fileHumanSize;  // 人类易读大小，如 "1.23 MB"，文件夹为空
    FILETIME createTime;     // 创建时间 "YYYY-MM-DD HH:MM:SS"
    FILETIME lastWriteTime;  // 最后修改时间
    FILETIME accessTime;     // 最后访问时间
    bool isReadOnly = false;  // 是否只读：文件可能为 true，文件夹始终为 false

    // 检查是否为空/无效对象
    bool empty() const {
        return objFullPath.empty();
    }

    // 检查对象是否存在（反义）
    bool exists() const {
        return !empty();
    }

    // ==================== 用于 std::set 排序 ====================
    bool operator<(const xyFileObject& other) const {
        // 主要按路径排序（路径唯一）
        if (objFullPath != other.objFullPath) {
            return objFullPath < other.objFullPath;
        }
        // 路径相同，按类型或其他字段（可选）
        return objType < other.objType;
    }
    // ================================================
};

// 辅助函数：格式化 FILETIME 到 wstring "YYYY-MM-DD HH:MM:SS"
std::wstring FormatFileTime(const FILETIME& ft) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    std::wstringstream ss;
    ss << std::setfill(L'0') << std::setw(4) << st.wYear << L"-"
        << std::setw(2) << st.wMonth << L"-"
        << std::setw(2) << st.wDay << L" "
        << std::setw(2) << st.wHour << L":"
        << std::setw(2) << st.wMinute << L":"
        << std::setw(2) << st.wSecond;
    return ss.str();
}

// 从 "YYYY-MM-DD HH:MM:SS" 格式的 wstring 解析回 FILETIME
FILETIME FormatFileTimeReverse(const std::wstring& timeStr)
{
    FILETIME ft = { 0, 0 };  // 默认返回无效时间（全零）

    if (timeStr.size() < 19) {  // 最小长度 "YYYY-MM-DD HH:MM:SS"
        return ft;
    }

    // 检查格式是否符合 "YYYY-MM-DD HH:MM:SS"
    if (timeStr[4] != L'-' || timeStr[7] != L'-' ||
        timeStr[10] != L' ' || timeStr[13] != L':' || timeStr[16] != L':') {
        return ft;
    }

    try {
        int year = std::stoi(timeStr.substr(0, 4));
        int month = std::stoi(timeStr.substr(5, 2));
        int day = std::stoi(timeStr.substr(8, 2));
        int hour = std::stoi(timeStr.substr(11, 2));
        int minute = std::stoi(timeStr.substr(14, 2));
        int second = std::stoi(timeStr.substr(17, 2));

        // 基本范围校验（FILETIME 有效范围：1601 ~ 30827）
        if (year < 1601 || year > 30827 ||
            month < 1 || month > 12 ||
            day < 1 || day > 31 ||
            hour < 0 || hour > 23 ||
            minute < 0 || minute > 59 ||
            second < 0 || second > 59) {
            return ft;
        }

        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(year);
        st.wMonth = static_cast<WORD>(month);
        st.wDay = static_cast<WORD>(day);
        st.wHour = static_cast<WORD>(hour);
        st.wMinute = static_cast<WORD>(minute);
        st.wSecond = static_cast<WORD>(second);
        st.wMilliseconds = 0;

        if (SystemTimeToFileTime(&st, &ft)) {
            return ft;  // 成功
        }
    }
    catch (...) {
        // std::stoi 失败等情况
    }

    return ft;  // 失败返回全零
}


// 辅助函数：人类易读文件大小
std::wstring HumanReadableSize(uint64_t size) {
    if (size == 0) return g_emptyWString;
    const wchar_t* units[] = { L"Byte", L"KB", L"MB", L"GB", L"TB", L"PB" };
    int unitIndex = 0;
    double dSize = static_cast<double>(size);
    while (dSize >= 1024 && unitIndex < 5) {
        dSize /= 1024;
        ++unitIndex;
    }
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << dSize << L" " << units[unitIndex];
    return ss.str();
}


// 检查文件是否有 Zone.Identifier 标记（windows接触锁定那个标记），目前不影响文件的操作
bool HasZoneIdentifier(const std::wstring& filePath) {
    std::wstring adsPath = filePath + L":Zone.Identifier";
    HANDLE hAds = CreateFileW(adsPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hAds == INVALID_HANDLE_VALUE) {
        return false;  // 无标记
    }
    CloseHandle(hAds);
    return true;
}

// 移除 Zone.Identifier 标记（解除锁定）
bool RemoveZoneIdentifier(const std::wstring& filePath) {
    std::wstring adsPath = filePath + L":Zone.Identifier";
    return DeleteFileW(adsPath.c_str()) != 0;
}


// 查询文件信息，获取 xyFileObject
xyFileObject GetFileObjectAttr(const std::wstring& path) {
    xyFileObject obj;
    if (path.empty()) {
        WriteFileSyncLog(L"路径为空，返回空对象", g_emptyWString, g_emptyWString, 5, 2, 3);  // 其它 操作，失败，警告
        return obj;
    }

    try {
        std::filesystem::path fsPath(path);
        if (!std::filesystem::exists(fsPath)) {
            WriteFileSyncLog(L"路径不存在，返回空对象", path, g_emptyWString, 5, 2, 2);  // 其它，失败，警告
            return obj;
        }

        // if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        //    DWORD err = GetLastError();
        //    WriteFileSyncLog(L"获取文件属性错误，错误码：" + std::to_wstring(err), path, g_emptyWString, 3, 2, 3);
        //    return xyFileObject{};
        //}

        obj.objFullPath = path;
        obj.pathType = (path.substr(0, 2) == L"\\\\" && path.substr(0, 4) != L"\\\\?\\") ? L"UNC" : L"Windows";

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            throw std::runtime_error("GetFileAttributesEx failed");
        }
        obj.objType = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? typeFOLDER : typeFILE;
        obj.isReadOnly = (obj.objType == typeFOLDER) ? false : (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        obj.objName = fsPath.filename().wstring();
        obj.fileSize = (obj.objType == typeFILE) ? (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow : 0;
        obj.fileHumanSize = (obj.objType == typeFILE) ? HumanReadableSize(obj.fileSize) : g_emptyWString;
        obj.createTime = fad.ftCreationTime;
        obj.lastWriteTime = fad.ftLastWriteTime;
        obj.accessTime = fad.ftLastAccessTime;
    }
    catch (...) {
        WriteFileSyncLog(L"获取对象属性异常，返回空对象", path, g_emptyWString, 5, 2, 4);  // 其它，失败，错误
        return xyFileObject{};  // 空对象
    }

    return obj;
}



// 获取 xyFileObject
// 【改造】：输入改为 set，返回所有路径的 xyFileObject 集合
std::set<xyFileObject> GetFileObjectAttrMulti(const std::set<std::wstring>& paths) {
    std::set<xyFileObject> results;

    for (const std::wstring& path : paths) {
        xyFileObject obj;
        if (path.empty()) {
            WriteFileSyncLog(L"路径为空，返回空对象", g_emptyWString, g_emptyWString, 5, 2, 3);  // 其它 操作，失败，警告
            results.insert(obj);
            continue;
        }

        try {
            std::filesystem::path fsPath(path);

            obj.objFullPath = path;
            obj.pathType = (path.substr(0, 2) == L"\\\\" && path.substr(0, 4) != L"\\\\?\\") ? L"UNC" : L"Windows";

            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
                throw std::runtime_error("GetFileAttributesEx failed");
            }
            obj.objType = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? typeFOLDER : typeFILE;
            obj.isReadOnly = (obj.objType == typeFOLDER) ? false : (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
            obj.objName = fsPath.filename().wstring();
            obj.fileSize = (obj.objType == typeFILE) ? (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow : 0;
            obj.fileHumanSize = (obj.objType == typeFILE) ? HumanReadableSize(obj.fileSize) : g_emptyWString;
            obj.createTime = fad.ftCreationTime;
            obj.lastWriteTime = fad.ftLastWriteTime;
            obj.accessTime = fad.ftLastAccessTime;
        }
        catch (...) {
            WriteFileSyncLog(L"获取对象属性异常，返回空对象", path, g_emptyWString, 5, 2, 4);  // 其它，失败，错误
            obj = xyFileObject{};  // 确保插入空对象
        }

        results.insert(obj);
    }

    return results;
}

// 基础方法：从路径获取名称（支持文件/文件夹、Windows/UNC）
std::wstring GetFileOrFolderName(const std::wstring& path) {
    if (path.empty()) return g_emptyWString;
    std::filesystem::path fsPath(path);
    return fsPath.filename().wstring();
}

// 基础方法：从路径获取除名称外的路径部分（支持文件/文件夹、Windows/UNC，最右带反斜杠）
std::wstring GetFileOrFolderPath(const std::wstring& path) {
    if (path.empty()) return g_emptyWString;
    std::filesystem::path fsPath(path);
    std::wstring parent = fsPath.parent_path().wstring();
    if (!parent.empty() && parent.back() != L'\\') parent += L'\\';
    return parent;
}


// 【刷新用于排除判断的全局缓存列表
// 目的：预计算所有需要特殊排除处理的 SyncDir 记录（singleFile 和 current 相关）
// 调用时机：g_syncDirPair 变更后（如加载配置、添加/删除目录对）调用此方法刷新缓存
void RefreshDiscardList()
{
    // 清空全局缓存（重新计算）
    syncDirPairHavingSingleFile.clear();
    syncDirPairHavingCurrent.clear();

    for (const auto& pair : g_syncDirPair) {
        // ==================== 处理 syncDirPairHavingSingleFile ====================
        bool hasSingleFileInner = false;
        if (pair.syncMode == syncModeSingleFile) {
            // 外层就是 singleFile 模式 → 直接加入
            syncDirPairHavingSingleFile.push_back(pair);
        }
        else {
            // 外层非 singleFile → 检查 innerPair 是否有任意 syncModeFile 记录
            for (const auto& inner : pair.innerPair) {
                if (inner.innerSyncMode == syncModeSingleFile) {
                    hasSingleFileInner = true;
                    break;
                }
            }
            if (hasSingleFileInner) {
                syncDirPairHavingSingleFile.push_back(pair);
            }
        }
        // ==================== 处理 syncDirPairHavingCurrent ====================
        if (pair.syncMode == syncModeCurrent) {
            // 外层就是 current 模式 → 直接加入，不再检查 innerPair
            syncDirPairHavingCurrent.push_back(pair);
        }
        else if (pair.syncMode == syncModeSingleFile) {
            // 外层是 singleFile → 直接跳过（不加入 current 列表）
            continue;
        }
        else if (pair.syncMode == syncModeFull) {
            // 外层是 full 模式 → 检查 innerPair 是否有任意 syncModeCurrent 记录
            bool hasCurrentInner = false;
            for (const auto& inner : pair.innerPair) {
                if (inner.innerSyncMode == syncModeCurrent) {
                    hasCurrentInner = true;
                    break;
                }
            }
            if (hasCurrentInner) {
                syncDirPairHavingCurrent.push_back(pair);
            }
        }
        // ================================================
    }

    // 可选：添加日志，便于调试
    // WriteFileSyncLog(L"RefreshDiscardList 完成：singleFile 相关记录 " + std::to_wstring(syncDirPairHavingSingleFile.size()) +
    //              L" 条，current 相关记录 " + std::to_wstring(syncDirPairHavingCurrent.size()) + L" 条", g_emptyWString, g_emptyWString, 5, 3, 1);
}





// 去除文件只读属性（仅对文件有效，文件夹无效）
void FileRemoveReadOnly(const std::wstring& srcFullPath)
{
    if (srcFullPath.empty()) {
        WriteFileSyncLog(L"路径为空，无法去除只读属性", srcFullPath, g_emptyWString, 5, 2, 3);  // 其它，失败，警告
        return;
    }
    try {
        std::filesystem::path fsPath(srcFullPath);
        // 如果路径不存在，直接日志并返回
        if (!std::filesystem::exists(fsPath)) {
            WriteFileSyncLog(L"路径不存在，无法去除只读属性", srcFullPath, g_emptyWString, 5, 2, 3);  // 其它，失败，警告
            return;
        }
        // 获取当前文件属性
        DWORD attributes = GetFileAttributesW(srcFullPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            WriteFileSyncLog(L"获取文件属性失败，无法去除只读", srcFullPath, g_emptyWString, 5, 2, 4);  // 其它，失败，错误
            return;
        }
        // 如果是文件夹（目录），只读属性无意义，直接返回（不视为错误）
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            WriteFileSyncLog(L"目标为文件夹，无需去除只读属性", srcFullPath, g_emptyWString, 5, 3, 2);  // 其它，其它，信息（正常情况）
            return;
        }
        // 如果当前不是只读，直接返回（避免不必要的系统调用）
        if (!(attributes & FILE_ATTRIBUTE_READONLY)) {
            WriteFileSyncLog(L"文件已非只读，无需操作", srcFullPath, g_emptyWString, 5, 3, 1);  // 其它，其它，调试
            return;
        }
        // 去除只读标志
        attributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!SetFileAttributesW(srcFullPath.c_str(), attributes)) {
            WriteFileSyncLog(L"去除只读属性失败", srcFullPath, g_emptyWString, 5, 2, 4);  // 其它，失败，错误
            return;
        }
        // 成功日志
        WriteFileSyncLog(L"成功去除文件只读属性", srcFullPath, g_emptyWString, 5, 1, 2);  // 其它，成功，信息
    }
    catch (...) {
        WriteFileSyncLog(L"去除只读属性过程中发生异常", srcFullPath, g_emptyWString, 5, 2, 4);  // 其它，失败，错误
    }
}

// 去除文件只读属性（仅对文件有效，文件夹无效）
// 【改造】：输入改为 set，逐一处理每个路径，单个逻辑完全不变
void FileRemoveReadOnlyMulti(const std::set<std::wstring>& srcFullPaths)
{
    for (const std::wstring& srcFullPath : srcFullPaths) {
        if (srcFullPath.empty()) {
            WriteFileSyncLog(L"路径为空，无法去除只读属性", srcFullPath, g_emptyWString, 5, 2, 3);  // 其它，失败，警告
            continue;
        }
        try {
            std::filesystem::path fsPath(srcFullPath);
            // 如果路径不存在，直接日志并返回
            if (!std::filesystem::exists(fsPath)) {
                WriteFileSyncLog(L"路径不存在，无法去除只读属性", srcFullPath, g_emptyWString, 5, 2, 3);  // 其它，失败，警告
                continue;
            }
            // 获取当前文件属性
            DWORD attributes = GetFileAttributesW(srcFullPath.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                WriteFileSyncLog(L"获取文件属性失败，无法去除只读", srcFullPath, g_emptyWString, 5, 2, 4);  // 其它，失败，错误
                continue;
            }
            // 如果是文件夹（目录），只读属性无意义，直接返回（不视为错误）
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                WriteFileSyncLog(L"目标为文件夹，无需去除只读属性", srcFullPath, g_emptyWString, 5, 3, 2);  // 其它，其它，信息（正常情况）
                continue;
            }
            // 如果当前不是只读，直接返回（避免不必要的系统调用）
            if (!(attributes & FILE_ATTRIBUTE_READONLY)) {
                WriteFileSyncLog(L"文件已非只读，无需操作", srcFullPath, g_emptyWString, 5, 3, 1);  // 其它，其它，调试
                continue;
            }
            // 去除只读标志
            attributes &= ~FILE_ATTRIBUTE_READONLY;
            if (!SetFileAttributesW(srcFullPath.c_str(), attributes)) {
                WriteFileSyncLog(L"去除只读属性失败", srcFullPath, g_emptyWString, 5, 2, 4);  // 其它，失败，错误
                continue;
            }
            // 成功日志
            WriteFileSyncLog(L"成功去除文件只读属性", srcFullPath, g_emptyWString, 5, 1, 2);  // 其它，成功，信息
        }
        catch (...) {
            WriteFileSyncLog(L"去除只读属性过程中发生异常", srcFullPath, g_emptyWString, 5, 2, 4);  // 其它，失败，错误
        }
    }
}



// 获取路径的父目录，并确保返回的路径不以 \ 结尾（标准化用于比较）
std::wstring GetParentPathNoBackSlash(const std::wstring& fullPath)
{
    if (fullPath.empty()) {
        return g_emptyWString;
    }
    try {
        std::filesystem::path fsPath(fullPath);

        // 如果是根路径（如 C:\ 或 \\server\share），parent_path() 可能为空或自身
        std::filesystem::path parent = fsPath.parent_path();
        if (parent.empty()) {
            return g_emptyWString;
        }
        std::wstring result = parent.wstring();
        // 强制去除末尾反斜杠（std::filesystem::path 通常会带，但我们业务需要无尾斜杠）
        if (!result.empty() && result.back() == L'\\') {
            result.pop_back();
        }
        return result;
    }
    catch (...) {
        WriteError(L"GetParentPathNoBackSlash 异常", fullPath, 4);
        return g_emptyWString;
    }
}


// 根据 srcFullPath 在 g_syncDirPair 中查找匹配的 listenPath，替换为 destSyncPath
// srcFullPath 和 listenPath 均不带末尾反斜杠
std::wstring GetPairedFullPathForSyncSingle(const std::wstring& srcFullPath)
{
    if (srcFullPath.empty()) {
        WriteFileSyncLog(L"GetPairedFullPathForSyncSingle：源路径为空，返回空字符串", srcFullPath, g_emptyWString, 5, 2, 3);
        return g_emptyWString;
    }

    try {
        std::wstring matchedDest;
        bool found = false;

        for (const auto& pair : g_syncDirPair) {
            // 使用 ContainSubFolderOrEqual 判断 srcFullPath 是否以 listenPath 开头（完整前缀匹配）
            if (ContainSubFolderOrEqual(srcFullPath, pair.listenPath)) {
                // 计算相对部分
                std::wstring relativePart = srcFullPath.substr(pair.listenPath.size());

                // 如果 listenPath 是 srcFullPath 的完整前缀，且相对部分不以反斜杠开头（正常情况）
                if (relativePart.empty() || relativePart[0] == L'\\') {
                    // 去掉可能的前导反斜杠
                    if (!relativePart.empty() && relativePart[0] == L'\\') {
                        relativePart = relativePart.substr(1);
                    }
                    // ==================== 修改：根据 syncMode 决定 result 初始值来源 ====================
                    // 如果 syncMode != syncModeSingleFile → 用 pair.destSyncPath
                    // 如果 syncMode == syncModeSingleFile → 循环 pair.innerPair 找匹配 innerSrcPath，用其 destPath
                    std::wstring result;
                    if (pair.syncMode != syncModeSingleFile) {
                        result = pair.destSyncPath;
                    }
                    else {
                        bool fileFound = false;
                        for (const auto& filePair : pair.innerPair) {
                            if (srcFullPath == filePair.innerSrcPath) {
                                result = filePair.innerDestPath;
                                fileFound = true;
                                break;
                            }
                        }
                        if (!fileFound) {
                            // 如果 singleFile 模式下没找到匹配文件 → 视为未找到 pair，继续循环其他 pair
                            continue;
                        }
                    }
                    // ================================================

                    if (!relativePart.empty()) {
                        if (!result.empty() && result.back() != L'\\') {
                            result += L'\\';
                        }
                        result += relativePart;
                    }
                    // 如果已经找到一条匹配（理论上不应多条，但以第一条为准）
                    if (!found) {
                        matchedDest = result;
                        found = true;
                    }
                    else {
                        // 多条匹配，记录警告（异常情况）
                        WriteFileSyncLog(L"GetPairedFullPathForSyncSingle：发现多条 listenPath 可匹配源路径，仅使用第一条",
                            srcFullPath, result, 5, 2, 3);
                    }
                }
            }
        }
        if (!found) {
            WriteFileSyncLog(L"GetPairedFullPathForSyncSingle：未找到匹配的 listenPath，路径无法映射",
                srcFullPath, g_emptyWString, 5, 2, 4);  // 错误级别
            return g_emptyWString;
        }
        return matchedDest;
    }
    catch (...) {
        WriteFileSyncLog(L"GetPairedFullPathForSyncSingle：转换过程中发生异常", srcFullPath, g_emptyWString, 5, 2, 4);
        return g_emptyWString;
    }
}


// 适用于具体的单个文件或目录，查找多目标，必须把type带进来作准确查询
// 根据 srcFullPath 在 g_syncDirPair 中查找匹配的 listenPath，替换为 destSyncPath
// srcFullPath 和 listenPath 均不带末尾反斜杠
// 【改造】：原返回单个目标路径，现返回 std::set<std::wstring> 包含所有可能的目标路径
std::set<std::wstring> GetAllPairedFullPathsForSync(const std::wstring& srcFullPath, const std::wstring& objType)
{
    std::set<std::wstring> resultSet;  // 【新增】：返回所有匹配的目标路径集合

    if (srcFullPath.empty()) {
        WriteFileSyncLog(L"GetAllPairedFullPathsForSync：源路径为空，返回空集合", srcFullPath, g_emptyWString, 5, 2, 3);
        return resultSet;  // 空集合
    }

    try {
        bool found = false;  // 【改造】：后续改为 resultSet 非空即为 true

        for (const auto& pair : g_syncDirPair) {
            // 使用 ContainSubFolderOrEqual 判断 srcFullPath 是否以 listenPath 开头（完整前缀匹配）
            if (ContainSubFolderOrEqual(srcFullPath, pair.listenPath)) {
                // 计算相对部分
                std::wstring relativePart = srcFullPath.substr(pair.listenPath.size());

                // 去掉可能的前导反斜杠
                if (!relativePart.empty() && relativePart[0] == L'\\') {
                    relativePart = relativePart.substr(1);
                }

                // ==================== 修改：根据 syncMode 收集所有可能的 result 初始值 ====================
                // 目标：非 singleFile 模式下收集 pair.destSyncPath + innerPair 中匹配的 destPath
                //       singleFile 模式下只收集 innerPair 中匹配的多个 destPath（不再 break）
                std::vector<std::wstring> candidateBases;

                // 外层只管目录级，满足条件的就加入base
                if (pair.syncMode == syncModeFull) candidateBases.push_back(pair.destSyncPath);
                if (pair.syncMode == syncModeCurrent && objType == typeFILE && GetParentPathNoBackSlash(srcFullPath) == pair.listenPath) candidateBases.push_back(pair.destSyncPath);
                // 外层不处理pair.syncMode == syncModeSingleFile情况，是因为这种情况全量的单文件记录（多目标）都在内层，外层这条冗余

                if (!pair.innerPair.empty()) {
                    // 【扩展】：遍历 innerPair，支持文件精确匹配 + 目录包含匹配
                    for (const auto& innerPair : pair.innerPair) {
                        bool match = false;

                        // 情况1：精确路径匹配，对于多目标来说有几条就匹配几条
                        if (innerPair.innerSyncMode == syncModeSingleFile && objType == typeFILE && srcFullPath == innerPair.innerSrcPath) {
                            candidateBases.push_back(innerPair.innerDestPath);
                        }
                        // 情况2：full模式，有几条就添加几条，同时适用于文件和目录
                        else if (innerPair.innerSyncMode == syncModeFull && ContainSubFolderOrEqual(srcFullPath, innerPair.innerSrcPath)) {
                            candidateBases.push_back(innerPair.innerDestPath);
                        }
                        // 情况3：current模式，只保留监听目录下的文件
                        else if (innerPair.innerSyncMode == syncModeCurrent && objType == typeFILE
                            && GetParentPathNoBackSlash(srcFullPath) == innerPair.innerSrcPath && ContainSubFolderOrEqual(srcFullPath, innerPair.innerSrcPath)) {
                            candidateBases.push_back(innerPair.innerDestPath);
                        }

                    }
                }
                // ================================================

                // 对每个 candidateBase 都进行相同的相对路径拼接
                for (const std::wstring& basePath : candidateBases) {
                    std::wstring result = basePath;

                    if (!relativePart.empty()) {
                        if (!result.empty() && result.back() != L'\\') {
                            result += L'\\';
                        }
                        result += relativePart;
                    }
                    resultSet.insert(result);
                    found = true;  // 只要有一个成功拼接，即视为找到
                }
            }
        }

        if (!found || resultSet.empty()) {
            // 无匹配不再是异常情况
            // WriteFileSyncLog(L"GetAllPairedFullPathsForSync：未找到匹配的 listenPath，路径无法映射", srcFullPath, g_emptyWString, 5, 2, 4);  
            return std::set<std::wstring>();  // 返回空集合
        }
        return resultSet;
    }
    catch (...) {
        WriteFileSyncLog(L"GetAllPairedFullPathsForSync：转换过程中发生异常", srcFullPath, g_emptyWString, 5, 2, 4);
        return std::set<std::wstring>();  // 异常时返回空集合
    }
}


// 新增方法：获取移动前后的多目标路径对集合
// 假设 srcMoveOld 和 srcMoveNew 在同一 listenPath 下（否则返回空）
// 返回 set<pair<old dest, new dest>>
std::set<std::pair<std::wstring, std::wstring>> GetPairedFullPathPairForMoveSync(const std::wstring& srcMoveOld, const std::wstring& srcMoveNew, const std::wstring& objType)
{
    std::set<std::pair<std::wstring, std::wstring>> resultPairs;

    if (srcMoveOld.empty() || srcMoveNew.empty()) {
        WriteFileSyncLog(L"GetPairedFullPathPairForMoveSync：源路径为空，返回空集合", srcMoveOld + L" to " + srcMoveNew, g_emptyWString, 5, 2, 3);
        return resultPairs;
    }

    try {

        // 满足move场景的两者，类型必然相同，调GetAllPairedFullPathsForSync获取
        // 直接取出源端路径变更前后两个文件分别对应的有效目录列表，已经体现了file、current、full在里面，exclude不在里面
        std::set<std::wstring> basesOld = GetAllPairedFullPathsForSync(srcMoveOld, objType);
        std::set<std::wstring> basesNew = GetAllPairedFullPathsForSync(srcMoveNew, objType);

        if (basesOld.empty() && basesNew.empty()) {
            WriteFileSyncLog(L"GetPairedFullPathPairForMoveSync：未找到匹配 bases", srcMoveOld, srcMoveNew, 5, 2, 4);
            return resultPairs;
        }

        // 转为 vector 以按索引访问
        std::vector<std::wstring> basesOldVec(basesOld.begin(), basesOld.end());
        std::vector<std::wstring> basesNewVec(basesNew.begin(), basesNew.end());

        // 按较大数量循环，核心思路：old为空表示new的多目标个数比old更多，反过来也一样，交给后面的move方法处理
        size_t maxSize = (std::max)(basesOldVec.size(), basesNewVec.size());
        std::vector<std::pair<std::wstring, std::wstring>> tempPairs;  // 临时 vector 用于排序
        for (size_t i = 0; i < maxSize; ++i) {
            std::wstring oldDest = (i < basesOldVec.size()) ? basesOldVec[i] : g_emptyWString;
            std::wstring newDest = (i < basesNewVec.size()) ? basesNewVec[i] : g_emptyWString;

            // 跳过双空
            if (!oldDest.empty() || !newDest.empty()) {
                tempPairs.emplace_back(oldDest, newDest);
            }
        }

        // 排序：双非空在前（按 !old.empty() && !new.empty() 降序）
        std::sort(tempPairs.begin(), tempPairs.end(), [](const auto& a, const auto& b) {
            bool aBothNonEmpty = !a.first.empty() && !a.second.empty();
            bool bBothNonEmpty = !b.first.empty() && !b.second.empty();
            if (aBothNonEmpty != bBothNonEmpty) {
                return aBothNonEmpty > bBothNonEmpty;  // true > false
            }
            // 同类，按 oldDest 字典序
            return a.first < b.first;
            });

        // 转回 set
        resultPairs.insert(tempPairs.begin(), tempPairs.end());

        if (resultPairs.empty()) {
            WriteFileSyncLog(L"GetPairedFullPathPairForMoveSync：无有效配对", srcMoveOld, srcMoveNew, 5, 2, 4);
        }

        return resultPairs;
    }
    catch (...) {
        WriteFileSyncLog(L"GetPairedFullPathPairForMoveSync：转换过程中发生异常", srcMoveOld, srcMoveNew, 5, 2, 4);
        return std::set<std::pair<std::wstring, std::wstring>>();
    }
}


//用于获取一个给定目录在全局监听目录中的根目录
std::wstring GetListenRootPath(const std::wstring& srcFullPath) {
    try {
        if (srcFullPath.empty()) {
            WriteError(L"方法GetListenRootPath入参srcFullPath非法", g_emptyWString, 3);
            return g_emptyWString;
        }
        for (const auto& pair : g_syncDirPair) {
            if (ContainSubFolderOrEqual(srcFullPath, pair.listenPath)) {
                return pair.listenPath;
            }
        }
    }
    catch (...) {
        WriteError(L"GetListenRootPath 发生异常", g_emptyWString, 4);
        return g_emptyWString;  // 异常时返回空
    }
    return g_emptyWString;
}


// 【判断是否需要排除，只检查g_listenExcludedFolders一个清单
// 只有在清单里时才返回true，其它所有时候都false
bool needExcludeByExcludeConfig(const std::wstring& srcFullPath) {
    if (g_listenExcludedFolders.empty()) {
        return false;
    }
    for (const auto& exclude : g_listenExcludedFolders) {
        if (ContainSubFolderOrEqual(srcFullPath, exclude)) {
            return true;
        }
    }
    return false;
}


// 【新增】：判断是否需要丢弃（current 模式下子目录树预处理）
// 默认返回 true（需要丢弃），只有匹配保留条件时返回 false
// 保留条件（任意一条匹配即保留）：
// a. syncModeFull + srcFullPath 是 listenPath 的子路径
// b. syncModeCurrent + parent == listenPath + objType == FILE
// c. innerPair 中任意一条满足 a 或 b（使用 innerSrcPath 替代 listenPath）
bool needDiscardByCurrentMode(const std::wstring& srcFullPath, const std::wstring& objType)
{
    // a0: 集合为空 → 无需丢弃，直接保留
    if (syncDirPairHavingCurrent.empty()) {
        return false;
    }

    std::wstring srcListenPath = GetListenRootPath(srcFullPath);
    if (srcListenPath.empty()) {
        // 无法确定归属的事件，一律丢弃
        return true;
    }
    bool inCurrentListenPath = false;
    std::wstring parent = GetParentPathNoBackSlash(srcFullPath);
    for (const auto& pairWithCurrent : syncDirPairHavingCurrent) {

        if (pairWithCurrent.listenPath == srcListenPath) {
            inCurrentListenPath = true;
            // a: 外层是 full 模式 + src 是 listenPath 的子路径（包括相等） → 保留
            if (pairWithCurrent.syncMode == syncModeFull && ContainSubFolderOrEqual(srcFullPath, pairWithCurrent.listenPath)) {
                return false;
            }

            // b: 外层是 current 模式 + 直接子对象是文件 → 保留
            if (pairWithCurrent.syncMode == syncModeCurrent && parent == pairWithCurrent.listenPath && objType == typeFILE) {
                return false;
            }
            // c: 检查 innerPair（如果非空）
            if (!pairWithCurrent.innerPair.empty()) {
                for (const auto& inner : pairWithCurrent.innerPair) {
                    // 内层 a: innerSyncMode == full + src 是 innerSrcPath 的子路径 → 保留
                    if (inner.innerSyncMode == syncModeFull && ContainSubFolderOrEqual(srcFullPath, inner.innerSrcPath)) {
                        return false;
                    }
                    // 内层 b: innerSyncMode == current + 直接子对象是文件 → 保留
                    if (inner.innerSyncMode == syncModeCurrent && parent == inner.innerSrcPath && objType == typeFILE) {
                        return false;
                    }
                }
            }
        }
    }
    // 循环完毕current相关的清单都没在里面，入参路径和current清单无关，从current角度不该被丢弃
    // 循环完毕如果在current清单里，但是没在需要保留的范围里，此时符合丢弃条件
    if (!inCurrentListenPath) {
        return false;
    }
    else {
        return true;
    }
}

// 【新增】：判断是否需要丢弃（singleFile 模式下子目录树及非指定文件预处理）
// 默认返回 true（需要丢弃），只有匹配保留条件时返回 false
// 保留条件（任意一条匹配即保留）：
// a. 集合为空 → 保留
// b. 外层 singleFile + innerPair 有任意 innerSyncMode==singleFile 且 innerSrcPath==srcFullPath → 保留
// c. 外层 full + src 是 listenPath 的子路径 → 保留
// d. 外层 current + parent==listenPath + objType==FILE → 保留
// e. 外层 full/current + innerPair 非空 → 内层检查 c/d（用 innerSrcPath 替代 listenPath）
bool needDiscardBySingleFileMode(const std::wstring& srcFullPath, const std::wstring& objType)
{
    // a: 集合为空 → 无需丢弃，直接保留
    if (syncDirPairHavingSingleFile.empty()) {
        return false;
    }

    std::wstring srcListenPath = GetListenRootPath(srcFullPath);
    if (srcListenPath.empty()) {
        // 无法确定归属的事件，一律丢弃
        return true;
    }
    bool inSingleFileListenPath = false;
    std::wstring parent = GetParentPathNoBackSlash(srcFullPath);

    for (const auto& pairWithSingleFile : syncDirPairHavingSingleFile) {
        if (pairWithSingleFile.listenPath == srcListenPath) {
            inSingleFileListenPath = true;
            // b: 外层是 singleFile 模式 → 检查 innerPair 是否有精确匹配的文件
            if (pairWithSingleFile.syncMode == syncModeSingleFile) {
                for (const auto& inner : pairWithSingleFile.innerPair) {
                    if (inner.innerSyncMode == syncModeSingleFile && srcFullPath == inner.innerSrcPath) {
                        return false;  // 精确匹配指定文件 → 保留
                    }
                }
            }
            // c: 外层是 full 模式 + src 是 listenPath 的子路径 → 保留
            if (pairWithSingleFile.syncMode == syncModeFull && ContainSubFolderOrEqual(srcFullPath, pairWithSingleFile.listenPath)) {
                return false;
            }
            // d: 外层是 current 模式 + 直接子对象是文件 → 保留
            if (pairWithSingleFile.syncMode == syncModeCurrent && parent == pairWithSingleFile.listenPath && objType == typeFILE) {
                return false;
            }
            // e: 检查 innerPair（如果非空）
            if (!pairWithSingleFile.innerPair.empty()) {
                for (const auto& inner : pairWithSingleFile.innerPair) {
                    // 内层 c: innerSyncMode == full + src 是 innerSrcPath 的子路径 → 保留
                    if (inner.innerSyncMode == syncModeFull && ContainSubFolderOrEqual(srcFullPath, inner.innerSrcPath)) {
                        return false;
                    }
                    // 内层 d: innerSyncMode == current + 直接子对象是文件 → 保留
                    if (inner.innerSyncMode == syncModeCurrent && parent == inner.innerSrcPath && objType == typeFILE) {
                        return false;
                    }
                }
            }
        }
    }
    // 循环完毕current相关的清单都没在里面，入参路径和current清单无关，从current角度不该被丢弃
    // 循环完毕如果在current清单里，但是没在需要保留的范围里，此时符合丢弃条件
    if (!inSingleFileListenPath) {
        return false;
    }
    else {
        return true;
    }
}

// 调GetAllPairedFullPathsForSync获取有效的目标个数
int countValidTargets(const std::wstring& srcFullPath, const std::wstring& objType) {
    if (srcFullPath.empty() || objType.empty()) return 0;
    std::set<std::wstring> targets = GetAllPairedFullPathsForSync(srcFullPath, objType);
    if (targets.empty() || targets.size() == 0) return 0;
    return static_cast<int>(targets.size());
}



// 创建目标目录（如果父目录不存在则报错；如果已存在则静默返回）
// windows原生方法，但没有处理过排除、丢弃，无法直接用于onload调用，会和配置不一致
void FolderCreate(const std::wstring& destFullPath)
{
    if (destFullPath.empty()) {
        WriteFileSyncLog(L"路径为空，无法创建目录", g_emptyWString, destFullPath, 5, 2, 3);  // 其它，失败，警告
        return;
    }

    try {
        std::filesystem::path dirPath(destFullPath);

        // 如果已经存在且是目录，直接返回（不视为错误）
        if (std::filesystem::exists(dirPath)) {
            if (std::filesystem::is_directory(dirPath)) {
                WriteFileSyncLog(L"目录已存在，无需创建", g_emptyWString, destFullPath, 5, 3, 2);  // 其它，其它，信息
                return;
            }
            else {
                WriteFileSyncLog(L"路径已存在但不是目录，无法创建", g_emptyWString, destFullPath, 5, 2, 4);  // 其它，失败，错误
                return;
            }
        }

        // 检查父目录是否存在（如果没有父目录则直接创建根目录是允许的）
        std::filesystem::path parent = dirPath.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            WriteFileSyncLog(L"父目录不存在，无法创建目标目录", g_emptyWString, destFullPath, 5, 2, 4);  // 其它，失败，错误
            return;
        }

        // 创建目录（单层，不递归）
        if (std::filesystem::create_directory(dirPath)) {
            WriteFileSyncLog(L"目录创建成功", g_emptyWString, destFullPath, 5, 1, 2);  // 其它，成功，信息
        }
        else {
            WriteFileSyncLog(L"目录创建失败（未知原因）", g_emptyWString, destFullPath, 5, 2, 4);  // 其它，失败，错误
        }
    }
    catch (...) {
        WriteFileSyncLog(L"创建目录过程中发生异常", g_emptyWString, destFullPath, 5, 2, 4);  // 其它，失败，错误
    }
}


// 入参updateOption：1，update_existing；2，overwrite_existing，3，skip_existing。默认值2
// windows原生方法，但没有处理过排除、丢弃，无法直接用于onload调用，会和配置不一致
void FileCopyOnly(const std::wstring& srcFullPath, const std::wstring& destFullPath, bool silent = true, bool recursive = false, int updateOption = 1)
{
    if (srcFullPath.empty() || destFullPath.empty()) {
        WriteFileSyncLog(L"源或目标路径为空", srcFullPath, destFullPath, 1, 2, 3);  // 新增，失败，警告
        return;
    }

    try {
        std::filesystem::path src(srcFullPath);
        std::filesystem::path dest(destFullPath);

        //if (!std::filesystem::exists(src)) {
        //    WriteFileSyncLog(L"源路径不存在", srcFullPath, destFullPath, 1, 2, 3);  // 新增，失败，警告
        //    return;
        //}

        // 目标父目录如不存在，自动创建（多层）
        if (dest.has_parent_path() && !std::filesystem::exists(dest.parent_path())) {
            std::filesystem::create_directories(dest.parent_path());
        }

        // 构建 copy_options
        std::filesystem::copy_options options = std::filesystem::copy_options::none;

        // 根据 updateOption 设置更新策略
        switch (updateOption) {
        case 1:  // update_existing：仅当源比目标新时覆盖
            options = std::filesystem::copy_options::update_existing;
            break;
        case 2:  // overwrite_existing：强制覆盖（不管新旧）
            options = std::filesystem::copy_options::overwrite_existing;
            break;
        case 3:  // skip_existing：跳过已存在文件
            options = std::filesystem::copy_options::skip_existing;
            break;
        default:
            WriteFileSyncLog(L"updateOption 参数非法（仅支持1-3），使用默认 overwrite_existing", srcFullPath, destFullPath, 1, 2, 3);
            options = std::filesystem::copy_options::overwrite_existing;
            break;
        }

        // 可选添加 recursive
        if (recursive) {
            options |= std::filesystem::copy_options::recursive;
        }

        // 执行拷贝
        std::filesystem::copy_file(src, dest, options);

        // 成功日志
        std::wstring modeDesc;
        switch (updateOption) {
        case 1: modeDesc = L"仅更新较新文件"; break;
        case 2: modeDesc = L"强制覆盖"; break;
        case 3: modeDesc = L"跳过已存在"; break;
        default: modeDesc = L"未知模式"; break;
        }
        WriteFileSyncLog(L"拷贝成功（" + modeDesc + (recursive ? L"，递归" : L"，非递归") + L"）",
            srcFullPath, destFullPath, 1, 1, 2);
    }
    catch (...) {

    }
}


// 拷贝文件/文件夹（同步，强制覆盖，支持文件/文件夹、Windows/UNC）
// 入参updateOption：1，update_existing；2，overwrite_existing，3，skip_existing。默认值2
// 【改造】：源路径为单个，目标路径为 set 集合，对每个目标路径执行一次完整拷贝
// 每个拷贝的内部逻辑与原版完全相同（updateOption、recursive、日志、异常处理等）
// windows原生方法，但没有处理过排除、丢弃，无法直接用于onload调用，会和配置不一致
void FileOrFolderCopy(const std::wstring& srcFullPath, const std::set<std::wstring>& destFullPaths, bool silent = true, bool recursive = true, int updateOption = 1)
{
    if (srcFullPath.empty() || destFullPaths.empty()) {
        WriteFileSyncLog(L"源路径为空或目标路径集合为空", srcFullPath, g_emptyWString, 1, 2, 3);  // 新增，失败，警告
        return;
    }

    for (const std::wstring& destFullPath : destFullPaths) {
        if (destFullPath.empty()) {
            WriteFileSyncLog(L"单个目标路径为空，跳过", srcFullPath, destFullPath, 1, 2, 3);  // 新增，失败，警告
            continue;
        }

        try {
            std::filesystem::path src(srcFullPath);
            std::filesystem::path dest(destFullPath);

            // 目标父目录如不存在，自动创建（多层）
            if (dest.has_parent_path() && !std::filesystem::exists(dest.parent_path())) {
                std::filesystem::create_directories(dest.parent_path());
            }

            // 构建 copy_options
            std::filesystem::copy_options options = std::filesystem::copy_options::none;

            // 根据 updateOption 设置更新策略
            switch (updateOption) {
            case 1:  // update_existing：仅当源比目标新时覆盖
                options = std::filesystem::copy_options::update_existing;
                break;
            case 2:  // overwrite_existing：强制覆盖（不管新旧）
                options = std::filesystem::copy_options::overwrite_existing;
                break;
            case 3:  // skip_existing：跳过已存在文件
                options = std::filesystem::copy_options::skip_existing;
                break;
            default:
                WriteFileSyncLog(L"updateOption 参数非法（仅支持1-3），使用默认 overwrite_existing", srcFullPath, destFullPath, 1, 2, 3);
                options = std::filesystem::copy_options::overwrite_existing;
                break;
            }

            // 可选添加 recursive
            if (recursive) {
                options |= std::filesystem::copy_options::recursive;
            }

            // 执行拷贝（单个路径逻辑完全不变）
            std::filesystem::copy(src, dest, options);

            // 成功日志
            std::wstring modeDesc;
            switch (updateOption) {
            case 1: modeDesc = L"仅更新较新文件"; break;
            case 2: modeDesc = L"强制覆盖"; break;
            case 3: modeDesc = L"跳过已存在"; break;
            default: modeDesc = L"未知模式"; break;
            }
            WriteFileSyncLog(L"拷贝成功（" + modeDesc + (recursive ? L"，递归" : L"，非递归") + L"）",
                srcFullPath, destFullPath, 1, 1, 2);
        }
        catch (...) {
            // 单个拷贝失败不中断整体，继续下一个目标路径
            WriteFileSyncLog(L"拷贝异常", srcFullPath, destFullPath, 1, 2, 4);
        }
    }
}



// 拷贝文件/文件夹（同步，强制覆盖，支持文件/文件夹、Windows/UNC）
// 入参updateOption：1，update_existing；2，overwrite_existing，3，skip_existing。默认值2
// windows原生方法，但没有处理过排除、丢弃，无法直接用于onload调用，会和配置不一致
void FileOrFolderCopySingle(const std::wstring& srcFullPath, const std::wstring& destFullPath, bool silent = true, bool recursive = true, int updateOption = 1)
{
    if (srcFullPath.empty() || destFullPath.empty()) {
        WriteFileSyncLog(L"源或目标路径为空", srcFullPath, destFullPath, 1, 2, 3);  // 新增，失败，警告
        return;
    }

    try {
        std::filesystem::path src(srcFullPath);
        std::filesystem::path dest(destFullPath);

        //if (!std::filesystem::exists(src)) {
        //    WriteFileSyncLog(L"源路径不存在", srcFullPath, destFullPath, 1, 2, 3);  // 新增，失败，警告
        //    return;
        //}

        // 目标父目录如不存在，自动创建（多层）
        if (dest.has_parent_path() && !std::filesystem::exists(dest.parent_path())) {
            std::filesystem::create_directories(dest.parent_path());
        }

        // 构建 copy_options
        std::filesystem::copy_options options = std::filesystem::copy_options::none;

        // 根据 updateOption 设置更新策略
        switch (updateOption) {
        case 1:  // update_existing：仅当源比目标新时覆盖
            options = std::filesystem::copy_options::update_existing;
            break;
        case 2:  // overwrite_existing：强制覆盖（不管新旧）
            options = std::filesystem::copy_options::overwrite_existing;
            break;
        case 3:  // skip_existing：跳过已存在文件
            options = std::filesystem::copy_options::skip_existing;
            break;
        default:
            WriteFileSyncLog(L"updateOption 参数非法（仅支持1-3），使用默认 overwrite_existing", srcFullPath, destFullPath, 1, 2, 3);
            options = std::filesystem::copy_options::overwrite_existing;
            break;
        }

        // 可选添加 recursive
        if (recursive) {
            options |= std::filesystem::copy_options::recursive;
        }

        // 执行拷贝
        std::filesystem::copy(src, dest, options);

        // 成功日志
        std::wstring modeDesc;
        switch (updateOption) {
        case 1: modeDesc = L"仅更新较新文件"; break;
        case 2: modeDesc = L"强制覆盖"; break;
        case 3: modeDesc = L"跳过已存在"; break;
        default: modeDesc = L"未知模式"; break;
        }
        WriteFileSyncLog(L"拷贝成功（" + modeDesc + (recursive ? L"，递归" : L"，非递归") + L"）",
            srcFullPath, destFullPath, 1, 1, 2);
    }
    catch (...) {

    }
}


// 批量拷贝文件/文件夹（源单个路径 → 目标多个路径）
// 输入：srcFullPath 为单个源路径，inputPaths 为目标路径集合
// 每个目标路径的处理逻辑与原 FileOrFolderCopyW 完全相同（包括所有排除判断、日志、updateOption 等）
// 无限递归指定目录为根目录下的所有子目录和文件，直至处理完毕
// 只有这里FileOrFolderCopyW经过了完美的排除、丢弃处理，和走队列的效果等价，只有这个文件操作接口可以用于onload，和file，current，full，多目标配置等价
BOOL FileOrFolderCopyW(
    const std::wstring& srcFullPath, const std::set<std::wstring>& inputPaths, bool silent = true, bool recursive = true, int updateOption = 1)  // 2 = 强制覆盖（推荐）
{
    if (srcFullPath.empty() || inputPaths.empty()) {
        WriteFileSyncLog(L"源路径为空或目标路径集合为空（批量拷贝）", srcFullPath, g_emptyWString, 1, 2, 3);
        return FALSE;
    }

    bool allSuccess = true;  // 【新增】：记录整体是否全部成功（任一失败即为 false）

    for (const std::wstring& destFullPath : inputPaths) {
        if (destFullPath.empty()) {
            WriteFileSyncLog(L"单个目标路径为空，跳过（批量拷贝）", srcFullPath, destFullPath, 1, 2, 3);
            allSuccess = false;
            continue;
        }

        try {
            // 1. 判断源类型（对每个目标都重新判断源属性，确保一致）
            DWORD srcAttr = GetFileAttributesW(srcFullPath.c_str());
            bool isDir = (srcAttr & FILE_ATTRIBUTE_DIRECTORY) != 0;

            // 【关键修复】：二次确认（防止 SMB 属性误判）
            if (isDir) {
                // 如果认为是目录，再用 filesystem 验证
                try {
                    if (!std::filesystem::is_directory(srcFullPath)) {
                        isDir = false;  // 属性说目录，但实际不是 → 强制视为文件
                        WriteFileSyncLog(L"属性显示目录，但 filesystem 确认是文件，强制处理为文件", srcFullPath, destFullPath, 1, 3, 1);
                    }
                }
                catch (...) {
                    // filesystem 异常 → 信任原始属性
                }
            }
            // 2. 处理文件夹（递归）
            if (isDir) {
                if (!recursive) {
                    WriteFileSyncLog(L"文件夹拷贝要求 recursive=true", srcFullPath, destFullPath, 1, 2, 3);
                    allSuccess = false;
                    continue;
                }

                // 预处理，由于single、current模式导致要丢弃的
                int countTargets = countValidTargets(srcFullPath, typeFOLDER);
                if (countTargets == 0) {
                    WriteFileSyncLog(L"因单文件、当前目录模式无匹配，丢弃本次目录操作", srcFullPath, destFullPath, 1, 3, 1);
                    continue;
                }

                // ==================== 检查当前目录是否匹配或包含任意例外目录 ====================
                if (needExcludeByExcludeConfig(srcFullPath)) {
                    WriteFileSyncLog(L"目录匹配例外规则，跳过拷贝及其子树", srcFullPath, destFullPath, 1, 3, 1);
                    continue;
                }
                // ================================================

                // 使用 SHCreateDirectoryExW 创建目标目录结构
                int result = SHCreateDirectoryExW(nullptr, destFullPath.c_str(), nullptr);
                if (result != ERROR_SUCCESS && result != ERROR_FILE_EXISTS && result != ERROR_ALREADY_EXISTS) {
                    WriteFileSyncLog(L"创建目标目录失败，错误码：" + std::to_wstring(result), srcFullPath, destFullPath, 1, 2, 4);
                    allSuccess = false;
                    continue;
                }

                // 递归遍历源目录（与原逻辑完全一致）
                WIN32_FIND_DATAW findData;
                HANDLE hFind = FindFirstFileW((srcFullPath + L"\\*").c_str(), &findData);

                if (hFind == INVALID_HANDLE_VALUE) {
                    DWORD err = GetLastError();
                    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES) {
                        WriteFileSyncLog(L"源目录为空，无内容需拷贝", srcFullPath, destFullPath, 1, 3, 1);
                        continue;
                    }
                    else {
                        WriteFileSyncLog(L"无法遍历源目录内容，错误码：" + std::to_wstring(err), srcFullPath, destFullPath, 1, 2, 4);
                        allSuccess = false;
                        continue;
                    }
                }

                do {
                    if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
                        continue;
                    }

                    std::wstring srcSub = srcFullPath + L"\\" + findData.cFileName;
                    std::wstring destSub = destFullPath + L"\\" + findData.cFileName;

                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        // 子目录：递归调用批量版
                        if (!FileOrFolderCopyW(srcSub, { destSub }, silent, true, updateOption)) {
                            FindClose(hFind);
                            allSuccess = false;
                            goto next_target;  // 跳出 do-while，继续下一个目标路径
                        }
                    }
                    else {
                        // 自身递归查询出子文件时
                        // 【改造】：子文件 → 调用 GetAllPairedFullPathsForSync 获取多目标集合，然后递归调用自身
                        std::set<std::wstring> subTargets = GetAllPairedFullPathsForSync(srcSub, typeFOLDER);
                        if (subTargets.empty()) {
                            WriteFileSyncLog(L"子文件未找到匹配目标路径，跳过拷贝", srcSub, g_emptyWString, 1, 2, 4);
                            allSuccess = false;
                            continue;
                        }
                        if (!FileOrFolderCopyW(srcSub, subTargets, silent, false, updateOption)) {  // 非递归（文件），使用多目标
                            FindClose(hFind);
                            allSuccess = false;
                            goto next_target;
                        }
                    }
                } while (FindNextFileW(hFind, &findData));

                FindClose(hFind);
                WriteFileSyncLog(L"文件夹拷贝成功（递归）", srcFullPath, destFullPath, 1, 1, 2);
                continue;

            next_target:
                continue;  // 继续下一个目标路径
            }

            // 3. 处理单个文件
            else {
                if (recursive) {
                    // 文件不需 recursive，但参数兼容，无影响
                }

                // 文件排除判断，这里要判断文件的父目录，因为例外规则配置的是目录
                std::wstring fileParent = GetParentPathNoBackSlash(srcFullPath);
                if (needExcludeByExcludeConfig(fileParent)) {
                    WriteFileSyncLog(L"文件父目录匹配例外规则，跳过拷贝", srcFullPath, destFullPath, 1, 3, 1);
                    continue;
                }

                // 预处理，由于single、current模式导致要丢弃的文件，在多目标情况下如果仍无匹配，则跳过
                int countTargets = countValidTargets(srcFullPath, typeFILE);
                if (countTargets == 0) {
                    WriteFileSyncLog(L"因单文件、当前目录模式无匹配，丢弃本次文件操作", srcFullPath, destFullPath, 1, 3, 1);
                    continue;
                }


                // updateOption 逻辑，和copy_file命令的options行为一致
                BOOL failIfExists = TRUE;
                if (updateOption == 1) {
                    xyFileObject srcObj = GetFileObjectAttr(srcFullPath);
                    xyFileObject destObj = GetFileObjectAttr(destFullPath);
                    if (destObj.exists() && CompareFileTime(&srcObj.lastWriteTime, &destObj.lastWriteTime) <= 0) {
                        WriteFileSyncLog(L"跳过拷贝（目标较新或相同）", srcFullPath, destFullPath, 1, 3, 1);
                        continue;
                    }
                    failIfExists = FALSE;
                }
                else if (updateOption == 2) {
                    failIfExists = FALSE;
                }
                else if (updateOption == 3) {
                    if (std::filesystem::exists(destFullPath)) {
                        WriteFileSyncLog(L"跳过拷贝（目标已存在）", srcFullPath, destFullPath, 1, 3, 1);
                        continue;
                    }
                    failIfExists = FALSE;
                }

                BOOL result = CopyFileW(srcFullPath.c_str(), destFullPath.c_str(), failIfExists);
                if (result) {
                    WriteFileSyncLog(L"文件拷贝成功", srcFullPath, destFullPath, 1, 1, 2);
                }
                else {
                    DWORD err = GetLastError();
                    WriteFileSyncLog(L"CopyFileW 失败，错误码：" + std::to_wstring(err), srcFullPath, destFullPath, 1, 2, 4);
                    allSuccess = false;
                }
            }
        }
        catch (...) {
            WriteFileSyncLog(L"FileOrFolderCopyW 未知异常（单个目标）", srcFullPath, destFullPath, 1, 2, 4);
            allSuccess = false;
        }
    }

    return allSuccess ? TRUE : FALSE;
}


// 原有单个文件目标的版本，可以和FileOrFolderCopyWIterate配套使用，在业务代码自身循环
// 注意，这批目录都没有处理过例外的部分，如果在onload使用则都无法处理！！！
BOOL FileOrFolderCopyWSingle(
    const std::wstring& srcFullPath, const std::wstring& destFullPath, bool silent = true, bool recursive = true, int updateOption = 1)  // 2 = 强制覆盖（推荐）
{
    if (srcFullPath.empty() || destFullPath.empty()) {
        WriteFileSyncLog(L"源或目标路径为空", srcFullPath, destFullPath, 1, 2, 3);
        return FALSE;
    }



    try {
        // 1. 判断源类型
        DWORD srcAttr = GetFileAttributesW(srcFullPath.c_str());
        bool isDir = (srcAttr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        // 【关键修复】：二次确认（防止 SMB 属性误判）
        if (isDir) {
            // 如果认为是目录，再用 filesystem 验证
            try {
                if (!std::filesystem::is_directory(srcFullPath)) {
                    isDir = false;  // 属性说目录，但实际不是 → 强制视为文件
                    WriteFileSyncLog(L"属性显示目录，但 filesystem 确认是文件，强制处理为文件", srcFullPath, destFullPath, 1, 3, 1);
                }
            }
            catch (...) {
                // filesystem 异常 → 信任原始属性
            }
        }

        // 2. 处理文件夹（递归）
        if (isDir) {
            if (!recursive) {
                WriteFileSyncLog(L"文件夹拷贝要求 recursive=true", srcFullPath, destFullPath, 1, 2, 3);
                return FALSE;
            }

            // 预处理，由于single、current模式导致要丢弃的文件，在多目标情况下如果仍无匹配，则跳过
            int countTargets = countValidTargets(srcFullPath, typeFOLDER);
            if (countTargets == 0) {
                WriteFileSyncLog(L"因单文件、当前目录模式无匹配，丢弃本次文件操作", srcFullPath, destFullPath, 1, 3, 1);
                return TRUE;
            }


            // ==================== 新增：检查当前目录是否匹配或包含任意例外目录 ====================
            // 如果 currDir (srcFullPath) 等于或以任意 g_listenExcludedFolders 开头，则跳过整个子树
            if (needExcludeByExcludeConfig(srcFullPath)) {
                // 匹配例外：不拷贝当前目录及其子树，直接返回 TRUE（视为成功跳过）
                WriteFileSyncLog(L"目录匹配例外规则，跳过拷贝及其子树", srcFullPath, destFullPath, 1, 3, 1);  // 可选日志
                return TRUE;
            }
            // ================================================

            // 使用 SHCreateDirectoryExW 创建目标目录结构（支持 UNC，自动创建多层）
            // FLAG: 0 = 默认行为
            int result = SHCreateDirectoryExW(nullptr, destFullPath.c_str(), nullptr);
            if (result != ERROR_SUCCESS &&
                result != ERROR_FILE_EXISTS &&
                result != ERROR_ALREADY_EXISTS) {
                WriteFileSyncLog(L"创建目标目录失败，错误码：" + std::to_wstring(result), srcFullPath, destFullPath, 1, 2, 4);
                return FALSE;
            }

            // 使用 CopyFileW 递归拷贝内容
            // 遍历源目录所有文件/子目录
            WIN32_FIND_DATAW findData;
            HANDLE hFind = FindFirstFileW((srcFullPath + L"\\*").c_str(), &findData);

            if (hFind == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES) {
                    // 空目录：没有文件可遍历，但目录已创建成功 → 正常返回
                    WriteFileSyncLog(L"源目录为空，无内容需拷贝", srcFullPath, destFullPath, 1, 3, 1);
                    return TRUE;  // 空目录拷贝成功
                }
                else {
                    WriteFileSyncLog(L"无法遍历源目录内容，错误码：" + std::to_wstring(err), srcFullPath, destFullPath, 1, 2, 4);
                    return FALSE;
                }
            }

            do {
                if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
                    continue;
                }

                std::wstring srcSub = srcFullPath + L"\\" + findData.cFileName;
                std::wstring destSub = destFullPath + L"\\" + findData.cFileName;

                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    // 子目录：递归调用
                    if (!FileOrFolderCopyWSingle(srcSub, destSub, silent, true, updateOption)) {
                        FindClose(hFind);
                        return FALSE;  // 子目录失败，整体失败
                    }
                }
                else {


                    // 文件：直接拷贝
                    BOOL copyOk = CopyFileW(srcSub.c_str(), destSub.c_str(), FALSE);  // FALSE = 允许覆盖
                    if (!copyOk) {
                        DWORD err = GetLastError();
                        WriteFileSyncLog(L"拷贝文件失败，错误码：" + std::to_wstring(err),
                            srcSub, destSub, 1, 2, 4);
                        FindClose(hFind);
                        return FALSE;
                    }
                }
            } while (FindNextFileW(hFind, &findData));

            FindClose(hFind);
            WriteFileSyncLog(L"文件夹拷贝成功（递归）", srcFullPath, destFullPath, 1, 1, 2);
            return TRUE;
        }

        // 3. 处理单个文件
        else {
            if (recursive) {
                // 文件不需 recursive，但参数兼容，无影响
            }

            // ==================== 新增：文件拷贝前检查其父目录是否在例外中 ====================
            // 获取文件父目录，检查是否匹配或包含任意例外目录
            std::wstring fileParent = GetParentPathNoBackSlash(srcFullPath);
            if (needExcludeByExcludeConfig(fileParent)) {
                // 匹配例外：不拷贝文件，直接返回 TRUE（视为成功跳过）
                WriteFileSyncLog(L"文件父目录匹配例外规则，跳过拷贝", srcFullPath, destFullPath, 1, 3, 1);  // 可选日志
                return TRUE;
            }
            // ================================================

            // 预处理，由于single、current模式导致要丢弃的文件，在多目标情况下如果仍无匹配，则跳过
            int countTargets = countValidTargets(srcFullPath, typeFILE);
            if (countTargets == 0) {
                WriteFileSyncLog(L"因单文件、当前目录模式无匹配，丢弃本次文件操作", srcFullPath, destFullPath, 1, 3, 1);
                return TRUE;
            }

            // updateOption 逻辑（文件场景）
            // 这里如果传入updateOption == 1则调用GetFileObjectAttr，又因为不知道调用方是新增还是修改场景，在新增时就直接报错2，
            // 暂时在新增场景里弃用，MOD还是只能靠它，因为标准接口copy命令搞不定
            BOOL failIfExists = TRUE;  // 默认不覆盖
            if (updateOption == 1) {  // update_existing：仅源更新的才覆盖
                xyFileObject srcObj = GetFileObjectAttr(srcFullPath);
                xyFileObject destObj = GetFileObjectAttr(destFullPath);
                if (destObj.exists() &&
                    CompareFileTime(&srcObj.lastWriteTime, &destObj.lastWriteTime) <= 0) {
                    // 目标更新的或相同 → 跳过
                    WriteFileSyncLog(L"跳过拷贝（目标较新或相同）", srcFullPath, destFullPath, 1, 3, 1);
                    return TRUE;  // 视为成功（符合 update 语义）
                }
                failIfExists = FALSE;  // 需要覆盖
            }
            else if (updateOption == 2) {  // overwrite_existing：强制覆盖
                failIfExists = FALSE;
            }
            else if (updateOption == 3) {  // skip_existing：跳过已存在
                if (std::filesystem::exists(destFullPath)) {
                    WriteFileSyncLog(L"跳过拷贝（目标已存在）", srcFullPath, destFullPath, 1, 3, 1);
                    return TRUE;
                }
                failIfExists = FALSE;
            }

            BOOL result = CopyFileW(srcFullPath.c_str(), destFullPath.c_str(), failIfExists);
            if (result) {
                WriteFileSyncLog(L"文件拷贝成功", srcFullPath, destFullPath, 1, 1, 2);
                return TRUE;
            }
            else {
                DWORD err = GetLastError();
                WriteFileSyncLog(L"CopyFileW 失败，错误码：" + std::to_wstring(err),
                    srcFullPath, destFullPath, 1, 2, 4);
                return FALSE;
            }
        }
    }
    catch (...) {
        // WriteFileSyncLog(L"FileOrFolderCopyWSingle 未知异常", srcFullPath, destFullPath, 1, 2, 4);
        return FALSE;
    }
}

// 【新增】：简单批量拷贝 - 仅迭代调用单路径版本 FileOrFolderCopyWSingle
// 无日志、异常捕获简单，适合性能敏感场景
// 注意，这批目录都没有处理过例外的部分，如果在onload使用则都无法处理！！！
void FileOrFolderCopyWIterate(
    const std::wstring& srcFullPath, const std::set<std::wstring>& inputPaths, bool silent = true, bool recursive = true, int updateOption = 1)
{
    if (srcFullPath.empty() || inputPaths.empty()) {
        return;
    }

    try {
        for (const std::wstring& destFullPath : inputPaths) {
            if (destFullPath.empty()) continue;

            // 直接调用单路径版本
            FileOrFolderCopyWSingle(srcFullPath, destFullPath, silent, recursive, updateOption);
        }
    }
    catch (...) {
        // 静默处理异常
    }
}




// 重命名文件/文件夹（支持文件/文件夹、Windows/UNC）
// 这批接口除了copyw外都不需要处理例外、丢弃（因current、file）的记录，因为都被队列处理了，只有copyw可能在onload需要用且由于onload不产生队列所以需要处理排除
void FileOrFolderRenameSingle(const std::wstring& destFullPath, const std::wstring& newName) {
    if (destFullPath.empty() || newName.empty()) {
        WriteFileSyncLog(L"路径或新名称为空", destFullPath, g_emptyWString, 3, 2, 3);  // 重命名，失败，警告
        return;
    }

    try {
        std::filesystem::path oldPath(destFullPath);
        // 删除这整段检查，因为unc存在延迟时不一定准
        // if (!std::filesystem::exists(oldPath)) {
        //     WriteFileSyncLog(L"改名前路径不存在", destFullPath, g_emptyWString, 3, 2, 3);
        //     return;
        // }

         //if (GetFileAttributesW(destFullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        //    DWORD err = GetLastError();
        //    WriteFileSyncLog(L"改名前路径无效，错误码：" + std::to_wstring(err), destFullPath, g_emptyWString, 3, 2, 3);
         //   return;
       // }

        std::filesystem::path newPath = oldPath.parent_path() / newName;
        std::filesystem::rename(oldPath, newPath);
        WriteFileSyncLog(L"重命名成功", destFullPath, newPath.wstring(), 3, 1, 2);  // 重命名，成功，信息
    }
    catch (...) {
        // WriteFileSyncLog(L"重命名异常", destFullPath, g_emptyWString, 3, 2, 3);  // 重命名，失败，错误
    }
}

// 【新增】：批量重命名文件/文件夹（支持文件/文件夹、Windows/UNC）
// 输入：多个目标路径（set），统一使用同一个 newName 进行重命名
// 每个路径的内部处理逻辑与 FileOrFolderRename 完全相同
// 这批接口除了copyw外都不需要处理例外、丢弃（因current、file）的记录，因为都被队列处理了，只有copyw可能在onload需要用且由于onload不产生队列所以需要处理排除
void FileOrFolderRename(const std::set<std::wstring>& destFullPaths, const std::wstring& newName) {
    if (destFullPaths.empty() || newName.empty()) {
        WriteFileSyncLog(L"路径集合为空或新名称为空（批量重命名）", g_emptyWString, g_emptyWString, 3, 2, 3);  // 重命名，失败，警告
        return;
    }

    for (const std::wstring& destFullPath : destFullPaths) {
        if (destFullPath.empty()) {
            WriteFileSyncLog(L"单个路径为空，跳过重命名", destFullPath, g_emptyWString, 3, 2, 3);  // 重命名，失败，警告
            continue;
        }

        try {
            std::filesystem::path oldPath(destFullPath);
            // 删除这整段检查，因为unc存在延迟时不一定准
            // if (!std::filesystem::exists(oldPath)) {
            //     WriteFileSyncLog(L"改名前路径不存在", destFullPath, g_emptyWString, 3, 2, 3);
            //     return;
            // }

            //if (GetFileAttributesW(destFullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            //    DWORD err = GetLastError();
            //    WriteFileSyncLog(L"改名前路径无效，错误码：" + std::to_wstring(err), destFullPath, g_emptyWString, 3, 2, 3);
            //    return;
            // }

            std::filesystem::path newPath = oldPath.parent_path() / newName;
            std::filesystem::rename(oldPath, newPath);
            WriteFileSyncLog(L"重命名成功", destFullPath, newPath.wstring(), 3, 1, 2);  // 重命名，成功，信息
        }
        catch (...) {
            // WriteFileSyncLog(L"重命名异常", destFullPath, g_emptyWString, 3, 2, 3);  // 重命名，失败，错误
            // 【保留原注释】单个重命名失败不中断整体，继续下一个路径
            WriteFileSyncLog(L"重命名异常（批量中单个）", destFullPath, g_emptyWString, 3, 2, 3);
        }
    }
}

// 移动文件或文件夹，单个
void FileOrFolderMoveSingle(const std::wstring& srcFullPath, const std::wstring& destFullPath) {
    if (srcFullPath.empty() || destFullPath.empty()) {
        WriteFileSyncLog(L"源或目标路径为空", srcFullPath, destFullPath, 3, 2, 3);  // 重命名/移动，失败，警告
        return;
    }

    try {
        // 使用 MoveFileExW：同分区零拷贝移动/重命名，跨分区自动 Copy+Delete + 覆盖存在
        BOOL result = MoveFileExW(srcFullPath.c_str(), destFullPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
        if (!result) {
            DWORD err = GetLastError();
            WriteFileSyncLog(L"MoveFileExW 失败，错误码：" + std::to_wstring(err), srcFullPath, destFullPath, 3, 2, 4);  // 失败，错误
            return;
        }
        WriteFileSyncLog(L"移动/重命名成功", srcFullPath, destFullPath, 3, 1, 2);  // 成功，信息
    }
    catch (...) {
        WriteFileSyncLog(L"移动/重命名异常", srcFullPath, destFullPath, 3, 2, 3);  // 失败，错误
    }
}

// 批量移动/重命名，支持配置 flags + 异步模式
// 在多目标情况下支持不同的模式，1、移动前后的路径都非空，直接移动；
// 2、移动前路径为空，从第一组已经移动后的路径复制给本组目标；3、移动后路径为空，直接删除本组的移动前路径的对象
// 批量移动/重命名，支持配置 flags + 异步模式
void FileOrFolderMove(const std::set<std::pair<std::wstring, std::wstring>>& pairs, DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED, bool async = false)
{
    if (pairs.empty()) {
        WriteFileSyncLog(L"批量移动对为空", g_emptyWString, g_emptyWString, 3, 2, 3);  // 失败，警告
        return;
    }

    // 转为 vector 排序：先处理 src/dst 都非空的 pair
    std::vector<std::pair<std::wstring, std::wstring>> sortedPairs(pairs.begin(), pairs.end());
    std::sort(sortedPairs.begin(), sortedPairs.end(), [](const auto& a, const auto& b) {
        bool aBothNonEmpty = !a.first.empty() && !a.second.empty();
        bool bBothNonEmpty = !b.first.empty() && !b.second.empty();
        if (aBothNonEmpty != bBothNonEmpty) {
            return aBothNonEmpty > bBothNonEmpty;  // 双非空在前
        }
        return a.first < b.first;  // 同类字典序
        });

    // 找到第一个 src 和 dst 都非空的 pair
    std::pair<std::wstring, std::wstring> firstPair = { g_emptyWString, g_emptyWString };
    bool foundFirst = false;
    for (const auto& p : sortedPairs) {
        if (!p.first.empty() && !p.second.empty()) {
            firstPair = p;
            foundFirst = true;
            break;
        }
    }

    auto processPair = [flags, &firstPair](const std::wstring& src, const std::wstring& dst) {
        // 校验：src 和 dst 不能同时空
        if (src.empty() && dst.empty()) {
            WriteFileSyncLog(L"单个移动对 src 和 dst 同时为空，跳过", src, dst, 3, 2, 3);
            return;
        }

        try {
            if (!src.empty() && !dst.empty()) {
                // src 和 dst 都非空：执行 MoveFileExW
                BOOL result = MoveFileExW(src.c_str(), dst.c_str(), flags);
                if (!result) {
                    DWORD err = GetLastError();
                    WriteFileSyncLog(L"MoveFileExW 失败，错误码：" + std::to_wstring(err), src, dst, 3, 2, 4);
                    return;
                }
                WriteFileSyncLog(L"批量移动/重命名成功", src, dst, 3, 1, 2);
            }
            else if (!src.empty() && dst.empty()) {
                // src 非空 dst 空：删除 src
                FileOrFolderDeleteSingle(src, true);
            }
            else if (src.empty() && !dst.empty()) {
                // src 空 dst 非空：从 firstPair.dst 复制到 dst
                if (firstPair.second.empty()) {
                    WriteFileSyncLog(L"无有效 firstPair.dst，无法复制到 dst", src, dst, 3, 2, 4);
                    return;
                }
                FileOrFolderCopyWSingle(firstPair.second, dst, true, true, fileCopyOption);  // 假设 updateOption=2 强制覆盖
            }
        }
        catch (...) {
            // WriteFileSyncLog(L"批量移动异常", src, dst, 3, 2, 3);
        }
        };

    if (async) {
        std::vector<std::thread> threads;
        for (const auto& p : sortedPairs) {
            threads.emplace_back(processPair, p.first, p.second);
        }
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }
    else {
        for (const auto& p : sortedPairs) {
            processPair(p.first, p.second);
        }
    }
}




// 删除文件/文件夹（强制、递归，支持文件/文件夹、Windows/UNC）
// 这批接口除了copyw外都不需要处理例外、丢弃（因current、file）的记录，因为都被队列处理了，只有copyw可能在onload需要用且由于onload不产生队列所以需要处理排除
void FileOrFolderDeleteSingle(const std::wstring& destFullPath, bool recursive = true) {
    if (destFullPath.empty()) {
        WriteFileSyncLog(L"路径为空", destFullPath, g_emptyWString, 4, 2, 3);  // 删除，失败，警告
        return;
    }
    try {
        std::filesystem::path path(destFullPath);


        // if (GetFileAttributesW(destFullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        //    DWORD err = GetLastError();
        //    WriteFileSyncLog(L"删除前路径不存在，错误码：" + std::to_wstring(err), destFullPath, g_emptyWString, 3, 2, 3);
        //     return;
        // }

        if (recursive) {
            std::filesystem::remove_all(path);
        }
        else {
            // 如果目标是文件夹且不递归，错误由windows os接管，会os异常
            std::filesystem::remove(path);
        }
        WriteFileSyncLog(L"删除成功", destFullPath, g_emptyWString, 4, 1, 2);  // 删除，成功，信息
    }
    catch (...) {
        WriteFileSyncLog(L"删除异常", destFullPath, g_emptyWString, 4, 2, 4);  // 删除，失败，错误
    }
}

// 删除文件/文件夹（强制、递归，支持文件/文件夹、Windows/UNC）
// 【改造】：输入改为 set，逐一处理每个路径，单个逻辑完全不变
// 这批接口除了copyw外都不需要处理例外、丢弃（因current、file）的记录，因为都被队列处理了，只有copyw可能在onload需要用且由于onload不产生队列所以需要处理排除
void FileOrFolderDelete(const std::set<std::wstring>& destFullPaths, bool recursive = true) {
    for (const std::wstring& destFullPath : destFullPaths) {
        if (destFullPath.empty()) {
            WriteFileSyncLog(L"路径为空", destFullPath, g_emptyWString, 4, 2, 3);  // 删除，失败，警告
            continue;
        }
        try {
            std::filesystem::path path(destFullPath);

            if (recursive) {
                std::filesystem::remove_all(path);
            }
            else {
                // 如果目标是文件夹且不递归，错误由windows os接管，会os异常
                std::filesystem::remove(path);
            }
            WriteFileSyncLog(L"删除成功", destFullPath, g_emptyWString, 4, 1, 2);  // 删除，成功，信息
        }
        catch (...) {
            WriteFileSyncLog(L"删除异常", destFullPath, g_emptyWString, 4, 2, 4);  // 删除，失败，错误
        }
    }
}


// 暂时注册失败的源 -> 目标目录对的重试方法，异步，走系统全局参数的重试间隔
// 如果执行之后全局参数里仍留有未成功记录，则下一次再次异步发起，如果清空了则整个服务运行期间不会再发起该方法
void RetrySyncDirPair(const int callType) {
    {
        if (callType != 1 && callType != 2) {
            std::wstring wmsg = L"RetrySyncDirPair调用参数非法，callType必须为1或2";
            WriteError(wmsg, g_emptyWString, 4);
            return;
        }
        StopUNCSession();
        StartUNCSession();
        std::lock_guard<std::mutex> lock(g_collectionMutex);
        auto& retry = g_syncDirPairRetry;
        bool retrySucceed = false;

        for (auto it = retry.begin(); it != retry.end(); ) {
            std::wstring lp = FormatPath(it->listenPath);
            std::wstring dp = FormatPath(it->destSyncPath);
            std::wstring sm = it->syncMode;
            std::set<std::wstring> initDest;



            if (DirectoryExistsWithCredential(lp, 1) && DirectoryExistsWithCredential(dp, 1)) {
                size_t countDest = 0;
                if (it->syncMode != syncModeSingleFile) ++countDest;
                if (!it->innerPair.empty()) countDest = countDest + it->innerPair.size();

                g_syncDirPair.push_back(*it);
                RegisterDir(lp);
                // 重试注册成功后该注册目录和onload逻辑一样
                initDest = GetAllPairedFullPathsForSync(lp, typeFOLDER);
                if (fullSyncOnLoad == 1) FileOrFolderCopyW(lp, initDest, true, true, fileCopyOptionOnLoad);
                retrySucceed = true;
                it = retry.erase(it);
                size_t seq = initDest.size();
                std::wstring wmsg = L"目录注册重试成功：" + lp + L"        ========> [" + std::to_wstring(seq) + L"#]        " + dp;
                WriteLog(wmsg, 5);
            }
            else {
                if (g_debugModeOnLoad) {
                    std::wstring wmsg = L"重试目录注册失败，源：" + lp + L"，目标：" + dp;
                    WriteError(wmsg, dp, 3);
                }
                ++it;
            }
        }
        // 结束时如果有重试成功记录，刷新一次包含file模式、current模式的清单到全局变量
        if (retrySucceed) RefreshDiscardList();
    }

    // 如果 retry 仍非空，且服务运行中，则x分钟后异步重试一次
    if (GetCollectionSize(SYNC_DIR_PAIR_RETRY) > 0 && g_running) {
        std::thread([]() {
            std::this_thread::sleep_for(g_retryInterval);
            if (g_running) {
                // RetrySyncDirPair();
                XYEventScheduler(RetryRegisterDirCtx, 1, EventProcessType::RetrySyncDirPair);
            }
            }).detach();
    }
}





void logCurrentAllLoadedFolders() {

    std::wstring logFilePath = logPath + L"/debug-all-folders.log";
    if (g_logLevel > 1) return;
    try {
        // 1. 确保目录存在
        fs::path p(logFilePath);
        if (p.has_parent_path() && !fs::exists(p.parent_path())) {
            fs::create_directories(p.parent_path());
        }

        std::ofstream ofs(logFilePath, std::ios::out | std::ios::trunc | std::ios::binary);

        if (ofs.is_open()) {
            // 5. 遍历集合并写入
            for (const auto& folderPath : g_listenAllFolders) {
                ofs << ToUTF8(FormatPath(folderPath)) << "\r\n";
            }

            ofs.flush();
            ofs.close();
            // std::wcout << L"已导出 " << g_listenAllFolders.size() << L" 条目录记录到 " << logFilePath << std::endl;
        }
        else {
            // 如果文件打开失败，通常是权限或路径被占用
        }
    }
    catch (const std::exception& e) {
        // 捕获可能的文件系统异常
        std::wstring wmsg = L"打印内存中所有目录清单发生未知异常：" + ToWide(e.what());
        WriteError(wmsg, logFilePath, 4);
    }
}

void logCurrentLoadedExcludedFolders() {

    if (g_logLevel > 1) return;
    std::wstring logFilePath = logPath + L"/debug-excluded-folders.log";

    try {
        // 1. 确保目录存在
        fs::path p(logFilePath);
        if (p.has_parent_path() && !fs::exists(p.parent_path())) {
            fs::create_directories(p.parent_path());
        }

        std::ofstream ofs(logFilePath, std::ios::out | std::ios::trunc | std::ios::binary);

        if (ofs.is_open()) {
            // 5. 遍历集合并写入
            for (const auto& folderPath : g_listenExcludedFolders) {
                ofs << ToUTF8(FormatPath(folderPath)) << "\r\n";
            }

            ofs.flush();
            ofs.close();
        }
        else {
            // 如果文件打开失败，通常是权限或路径被占用
        }
    }
    catch (const std::exception& e) {
        // 捕获可能的文件系统异常
        std::wstring wmsg = L"打印内存中例外目录清单发生未知异常：" + ToWide(e.what());
        WriteError(wmsg, logFilePath, 4);
    }
}


void logCurrentDirPairs(int pairType)
{
    if (g_logLevel > 1) return;
    try {
        // ===== 1. 根据入参选择集合和文件名 =====
        const std::vector<SyncDir>* targetVec = nullptr;
        std::wstring syncMode;
        std::wstring fileName;

        switch (pairType) {
        case SYNC_DIR_PAIR:
            targetVec = &g_syncDirPair;
            fileName = L"debug-registered-folders.log";
            break;
        case SYNC_DIR_PAIR_RETRY:
            targetVec = &g_syncDirPairRetry;
            fileName = L"debug-retry-folders.log";
            break;
        default:
            WriteError(L"logCurrentDirPairs 失败：未知 pairType", g_emptyWString, 3);
            return;
        }

        if (!targetVec) return;

        // ===== 2. 遍历集合，将每条记录转换为一行字符串 =====
        std::vector<std::wstring> lines;
        for (const auto& pair : *targetVec) {
            // 拼接 listenPath 和 destSyncPath，用 " ========> " 分隔
            std::wstring line = FormatPath(pair.listenPath) + L"        ========>        " + FormatPath(pair.destSyncPath) + L"        --------        模式：" + pair.syncMode;
            if (!pair.innerPair.empty()) {
                line = line + L"    内层目录对如下";
                for (const auto& inner : pair.innerPair) {
                    line = line + L"，源（" + inner.innerSyncMode + L"）：" + inner.innerSrcPath + L"、目标：" + inner.innerDestPath;
                }
            }
            lines.push_back(line);
        }


        if (!syncDirPairHavingCurrent.empty()) {
            lines.push_back(L"\r\n\r\n");
            lines.push_back(L"syncDirPairHavingCurrent清单：");
            for (const auto& pair : syncDirPairHavingCurrent) {
                std::wstring line = L"(" + pair.syncMode + L"): " + pair.listenPath + L"    ====>    " + pair.destSyncPath;
                if (!pair.innerPair.empty()) {
                    line = line + L"    内层目录对如下";
                    for (const auto& inner : pair.innerPair) {
                        line = line + L"，源（" + inner.innerSyncMode + L"）：" + inner.innerSrcPath + L"、目标：" + inner.innerDestPath;
                    }
                }
                lines.push_back(line);
            }
        }

        if (!syncDirPairHavingSingleFile.empty()) {
            lines.push_back(L"\r\n\r\n");
            lines.push_back(L"syncDirPairHavingSingleFile清单：");
            for (const auto& pair : syncDirPairHavingSingleFile) {
                std::wstring line = L"(" + pair.syncMode + L"): " + pair.listenPath + L"    ====>    " + pair.destSyncPath;
                if (!pair.innerPair.empty()) {
                    line = line + L"    内层目录对如下";
                    for (const auto& inner : pair.innerPair) {
                        line = line + L"，源（" + inner.innerSyncMode + L"）：" + inner.innerSrcPath + L"、目标：" + inner.innerDestPath;
                    }
                }
                lines.push_back(line);
            }
        }


        // ===== 3. 调用 WriteUTF8LinesToFile 写入文件 =====
        WriteUTF8LinesToFile(logPath + L"/" + fileName, &g_logMutex, lines, 1);  // 覆盖写入
    }
    catch (const std::exception& e) {
        std::wstring wmsg = L"logCurrentDirPairs 发生异常：" + ToWide(e.what());
        WriteError(wmsg, g_emptyWString, 4);
    }
    catch (...) {
        WriteError(L"logCurrentDirPairs 发生未知异常", g_emptyWString, 4);
    }
}






// 获取status里的重试次数，无法解析的情况下默认返回0
int GetQueueRetryTimes(std::wstring input) {
    try {
        // 健壮性检查：如果字符串为空，直接返回 0
        if (input.empty()) {
            return 0;
        }
        // 1. 尝试查找最后一个逗号
        size_t lastCommaPos = input.find_last_of(L',');

        if (lastCommaPos != std::wstring::npos) {
            // 取最后一个逗号后的文本
            std::wstring lastPart = input.substr(lastCommaPos + 1);

            // Trim 前后空格
            size_t first = lastPart.find_first_not_of(L" \t\r\n");
            if (first != std::wstring::npos) {
                size_t last = lastPart.find_last_not_of(L" \t\r\n");
                std::wstring trimmedPart = lastPart.substr(first, (last - first + 1));

                try {
                    // 尝试转化为 int
                    return std::stoi(trimmedPart);
                }
                catch (...) {
                    // 转化失败，不做操作，继续向下走“取最后一个字符”的逻辑
                }
            }
        }
        // 2. 如果没有逗号，或者逗号后的文本转化失败：取整个入参文本最后一个字符
        wchar_t lastChar = input.back();
        std::wstring sLastChar(1, lastChar);
        try {
            return std::stoi(sLastChar);
        }
        catch (...) {
            // 最后一个字符也无法转化
            return 0;
        }
    }
    catch (...) {
        // 捕获任何可能的内存访问或其他异常
        return 0;
    }
}





/* ================= 全局容器，用于 STOP 清理 ================= */
std::vector<DirContext*> g_allDirs;
std::mutex g_dirMutex;

// 入参是文件路径，返回该文件全文宽字符串
std::wstring GetFileContent(const std::wstring& path)
{
    auto raw = ReadAllBytes(path);
    if (raw.empty()) {
        std::wstring wmsg = L"文件不存在或内容为空，跳过";
        WriteError(wmsg, path, 3);
        return g_emptyWString;
    }

    const uint8_t* data = raw.data();
    size_t size = raw.size();

    // 1. UTF-16 LE BOM
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(data + 2), (size - 2) / 2);
    }

    // 2. UTF-16 BE BOM
    if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        std::wstring result;
        result.reserve((size - 2) / 2);
        for (size_t i = 2; i < size; i += 2) {
            result.push_back((data[i] << 8) | data[i + 1]);
        }
        return result;
    }

    // 3. UTF-8 BOM
    size_t offset = 0;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        offset = 3;
    }

    // 4. 先严格尝试 UTF-8（无 BOM 也包括进来）
    int utf8Len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(data + offset),
        static_cast<int>(size - offset), nullptr, 0);
    if (utf8Len > 0) {
        std::wstring result(utf8Len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0,
            reinterpret_cast<const char*>(data + offset),
            static_cast<int>(size - offset),
            &result[0], utf8Len);
        return result;                     // UTF-8 成功 → 直接返回，最优先
    }

    // 5. UTF-8 失败，才尝试 ANSI（CP_ACP，通常是 GBK）
    std::string ansiBytes(reinterpret_cast<const char*>(data + offset), size - offset);
    int ansiLen = MultiByteToWideChar(CP_ACP, 0, ansiBytes.data(), static_cast<int>(ansiBytes.size()), nullptr, 0);
    if (ansiLen > 0) {
        std::wstring result(ansiLen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, ansiBytes.data(), static_cast<int>(ansiBytes.size()),
            &result[0], ansiLen);
        return result;
    }

    // 6. 都失败，返回空
    return g_emptyWString;
}




// 判断是否属于“首尾应被清理的空白/控制字符”
inline bool IsTrimChar(wchar_t ch)
{
    switch (ch)
    {
    case L' ':
    case L'\t':
    case L'\r':
    case L'\n':
    case L'\f':
    case L'\v':
    case 0xFEFF: // ★ BOM，关键
        return true;
    default:
        return iswspace(ch);
    }
}


void FormatLine(std::wstring& line)
{
    if (line.empty())
        return;

    size_t begin = 0;
    size_t end = line.size();

    // 剔除首部
    while (begin < end && IsTrimChar(line[begin]))
        ++begin;

    // 剔除尾部
    while (end > begin && IsTrimChar(line[end - 1]))
        --end;

    if (begin >= end)
    {
        line.clear();
        return;
    }

    // 就地修改
    if (begin > 0 || end < line.size())
    {
        line.assign(line.substr(begin, end - begin));
    }
}

// 格式化wstring字符串，返回新字符串
std::wstring FormattedLine(const std::wstring& line)
{
    std::wstring input = line;
    if (input.empty())
        return g_emptyWString;

    size_t begin = 0;
    size_t end = input.size();

    // 从头部剔除
    while (begin < end && IsTrimChar(input[begin]))
        ++begin;

    // 从尾部剔除
    while (end > begin && IsTrimChar(input[end - 1]))
        --end;

    if (begin >= end)
        return g_emptyWString;

    return input.substr(begin, end - begin);
}

// 解析一行文本后的元素容器（最多支持30个元素）
struct LineElements {
    std::wstring element1; std::wstring element2; std::wstring element3; std::wstring element4; std::wstring element5; std::wstring element6; std::wstring element7;
    std::wstring element8; std::wstring element9;  std::wstring element10; std::wstring element11; std::wstring element12; std::wstring element13;
    std::wstring element14; std::wstring element15; std::wstring element16; std::wstring element17; std::wstring element18; std::wstring element19;
    std::wstring element20; std::wstring element21; std::wstring element22; std::wstring element23; std::wstring element24; std::wstring element25;
    std::wstring element26; std::wstring element27; std::wstring element28; std::wstring element29; std::wstring element30;
    int count = 0;  // 当前有效元素数量（非空或已解析的）
    // 是否一个元素都没有
    bool empty() const {
        return count == 0;
    }
    // 是否至少有一个元素（反义）
    bool exists() const {
        return count > 0;
    }
};

// 解析行文本，按指定分隔符（成对出现）提取所有元素，最多30个
LineElements ReadLineElements(const std::wstring& lineText, const wchar_t separator)
{
    LineElements result;

    try {
        std::wstring input = FormattedLine(lineText);

        if (input.empty()) {
            return result;  // count=0, empty()=true
        }
        // 校验分隔符是否合法
        const std::wstring ALLOWED_DELIMITERS = L",;\?/|\"'-";
        if (separator == L'\0' ||
            ALLOWED_DELIMITERS.find(separator) == std::wstring::npos)
        {
            std::wstring wmsg = L"ReadLineElements分隔符非法或不在限定支持范围内：" + std::to_wstring(static_cast<int>(separator));
            WriteError(wmsg, g_emptyWString, 4);
            return result;
        }

        // 统计分隔符数量，必须成对
        int quoteCount = static_cast<int>(CountChar(input, separator));
        if (quoteCount % 2 != 0) {
            std::wstring wmsg = L"ReadLineElements解析行文本异常，分隔符不成对，数量："
                + std::to_wstring(quoteCount)
                + L"，行文本：" + input;
            WriteError(wmsg, input, 3);
            return result;
        }

        // 校验是否包含换行符
        if (input.find_first_of(L"\r\n") != std::wstring::npos) {
            std::wstring wmsg = L"ReadLineElements解析行文本异常，文本包含换行符，行：" + input;
            WriteError(wmsg, input, 3);
            return result;
        }
        // 开始解析：按成对分隔符提取内容
        std::vector<std::wstring> elements;
        size_t pos = 0;
        while (pos < input.size()) {
            // 找到起始分隔符
            if (input[pos] != separator) {
                ++pos;
                continue;
            }
            ++pos; // 跳过起始分隔符

            std::wstring current;
            while (pos < input.size() && input[pos] != separator) {
                current.push_back(input[pos]);
                ++pos;
            }
            // 规整下前后无意义字符
            current = FormattedLine(current);
            elements.push_back(current);

            if (pos < input.size()) {
                ++pos; // 跳过结束分隔符
            }
        }

        // 填充到 LineElements，最多30个
        int maxElements = 30;
        result.count = static_cast<int>(
            elements.size() < static_cast<size_t>(maxElements)
            ? elements.size()
            : maxElements
            );

        if (elements.size() > maxElements) {
            WriteLog(L"ReadLineElements输入行解析出的总元素数量大于30，超出的部分被丢弃", 2);
        }

        for (int i = 0; i < result.count; ++i) {
            switch (i) {
            case 0:  result.element1 = elements[i]; break;
            case 1:  result.element2 = elements[i]; break;
            case 2:  result.element3 = elements[i]; break;
            case 3:  result.element4 = elements[i]; break;
            case 4:  result.element5 = elements[i]; break;
            case 5:  result.element6 = elements[i]; break;
            case 6:  result.element7 = elements[i]; break;
            case 7:  result.element8 = elements[i]; break;
            case 8:  result.element9 = elements[i]; break;
            case 9:  result.element10 = elements[i]; break;
            case 10: result.element11 = elements[i]; break;
            case 11: result.element12 = elements[i]; break;
            case 12: result.element13 = elements[i]; break;
            case 13: result.element14 = elements[i]; break;
            case 14: result.element15 = elements[i]; break;
            case 15: result.element16 = elements[i]; break;
            case 16: result.element17 = elements[i]; break;
            case 17: result.element18 = elements[i]; break;
            case 18: result.element19 = elements[i]; break;
            case 19: result.element20 = elements[i]; break;
            case 20: result.element21 = elements[i]; break;
            case 21: result.element22 = elements[i]; break;
            case 22: result.element23 = elements[i]; break;
            case 23: result.element24 = elements[i]; break;
            case 24: result.element25 = elements[i]; break;
            case 25: result.element26 = elements[i]; break;
            case 26: result.element27 = elements[i]; break;
            case 27: result.element28 = elements[i]; break;
            case 28: result.element29 = elements[i]; break;
            case 29: result.element30 = elements[i]; break;
            }
        }
    }
    catch (...) {
        WriteError(L"ReadLineElements解析过程中发生异常", g_emptyWString, 4);
        // result 保持默认（count=0，所有element为空）
    }
    return result;
}



// 解析行文本，按双引号分隔，返回指定索引的元素（从1开始）
std::wstring ReadLineElementByIndex(const std::wstring lineText, const int index, const wchar_t separator) {
    try {

        std::wstring input = FormattedLine(lineText);

        if (input.empty()) {
            return g_emptyWString;
        }


        // 校验 delimiter 是否合法（只能是单个字符，且在允许集合中）
        const std::wstring ALLOWED_DELIMITERS = L",;\?/|\"'-";
        if (separator == L'\0' ||                     // 空字符
            ALLOWED_DELIMITERS.find(separator) == std::wstring::npos)
        {
            // 校验失败，返回空向量（或你可以返回一个特殊值）
            std::wstring wmsg = L"分隔符非法或不在限定支持范围内：" + std::to_wstring(separator);
            WriteError(wmsg, g_emptyWString, 4);
            return g_emptyWString;
        }

        // 统计分隔符数量，必须成对出现。第一句是显式static_cast<int>，避免超过 INT_MAX
        int quoteCount = static_cast<int>(CountChar(input, separator));
        // int quoteCount = CountChar(input, separator);
        if (quoteCount % 2 != 0) {
            std::wstring wmsg = L"解析行文本异常，分隔符不成对，数量：" + std::to_wstring(quoteCount) + L"，index：" + std::to_wstring(index) + L"，分割符：" + separator;
            WriteError(wmsg, input, 4);
            return g_emptyWString;
        }

        // 校验是否包含换行符
        if (input.find_first_of(L"\r\n") != std::wstring::npos) {
            std::wstring wmsg = L"解析行文本异常，文本包含换行符，行：" + input;
            WriteError(wmsg, input, 4);
            return g_emptyWString;
        }

        std::vector<std::wstring> elements;
        size_t pos = 0;
        while (pos < input.size()) {
            if (input[pos] != separator) {
                ++pos;
                continue;
            }
            ++pos; // 跳过起始 "
            std::wstring current;
            while (pos < input.size() && input[pos] != separator) {
                current.push_back(input[pos]);
                ++pos;
            }
            elements.push_back(current);
            ++pos; // 跳过结束 "
        }

        if (index > 0 && index <= (int)elements.size()) {
            return elements[index - 1];
        }
        return g_emptyWString;
    }

    catch (...) {
        std::wstring wmsg = L"解析行文本异常";
        WriteError(wmsg, g_emptyWString, 4);
        return g_emptyWString;
    }
}




struct ListenConfigRow {
    std::wstring listenType;
    std::wstring sourceFolder;
    std::wstring destFolder;
    std::wstring syncMode;
    std::vector<std::wstring> exclusion;
    std::wstring originalLine;
};

ListenConfigRow ParseListenConfigLine(const std::wstring& line) {
    ListenConfigRow r;
    LineElements le = ReadLineElements(line, g_delimiter);
    r.listenType = le.element1;
    r.sourceFolder = le.element2;
    r.destFolder = le.element3;
    r.syncMode = le.element4;
    if (le.element4 == syncModeCurrent || le.element4 == syncModeFull) {
        r.syncMode = le.element4;
    }
    else {
        // 默认值为full，包括配置空值
        r.syncMode = syncModeFull;
    }
    if (le.element1 == typeFILE) r.syncMode = syncModeSingleFile;
    r.exclusion = SplitByDelimitedField(le.element5, L'|');
    r.originalLine = line;
    return r;
}


// 构造队列数据的结构，因为这是文件监听触发源、后续文件处理的核心数据
struct QueueRow {
    std::wstring action;
    std::wstring objType;
    std::wstring objName;
    std::wstring objFullPath;
    std::wstring timestamp;
    std::wstring eventId;
    std::wstring status;
    std::wstring originalLine;
    std::wstring parentPath;  // 所属父目录路径（标准化：无末尾 \），仅文件有效；文件夹留空
    // 仅用于 set 去重，按完整路径比较
    bool operator<(const QueueRow& rhs) const {
        return objFullPath < rhs.objFullPath;
    }
    // 【新增】：判断该 QueueRow 是否为空对象（未成功删除或未找到）
    bool empty() const {
        // 只要关键字段为空，就视为空对象
        return objFullPath.empty() &&
            objName.empty() &&
            timestamp.empty() &&
            action.empty() &&
            eventId.empty();
    }
};



QueueRow ParseQueueLine(const std::wstring& line) {
    QueueRow r;
    LineElements le = ReadLineElements(line, g_delimiter);
    r.action = le.element1;
    r.objType = le.element2;
    r.objName = le.element3;
    r.objFullPath = le.element4;
    r.timestamp = le.element5;
    r.eventId = le.element6;
    r.status = le.element7;
    r.originalLine = line;
    r.parentPath = (r.objType == typeFOLDER) ? g_emptyWString : GetParentPathNoBackSlash(r.objFullPath);
    return r;
}

// 维护一个数据结构，用于区分文件和文件夹两套set，按操作类型进一步分类
struct QueueSet {
    // 文件夹分类 set
    std::set<QueueRow> folderAddSet;     // action == actionADD 或 actionADDR 的文件夹
    std::set<QueueRow> folderDeleteSet;  // action == actionDEL 或 actionDELR 的文件夹
    std::set<QueueRow> otherSet;   // 改造为other包含file和folder，原因是整体需要按时间顺序处理

    // 文件分类 set
    std::set<QueueRow> fileAddSet;       // action == actionADD 或 actionADDR 的文件
    std::set<QueueRow> fileDeleteSet;    // action == actionDEL 或 actionDELR 的文件
    //std::set<QueueRow> fileOtherSet;     // 其他 action 的文件

    bool empty() const {
        return folderAddSet.empty() && folderDeleteSet.empty() && otherSet.empty() &&
            fileAddSet.empty() && fileDeleteSet.empty();
    }

    bool exists() const {
        return !empty();
    }
};


// 从 QueueSet 中提取按目录层级排序的文件夹 QueueRow 列表（保留完整对象）
// mode: 1 = 新增目录（folderAddSet），从浅到深排序
//       2 = 删除目录（folderDeleteSet），从深到浅排序
std::vector<QueueRow> GetSortedFolderListFromQueueSet(const QueueSet& qs, int mode = 1)
{
    std::vector<QueueRow> sortedFolders;

    const std::set<QueueRow>* sourceSet = nullptr;

    if (mode == 1) {
        sourceSet = &qs.folderAddSet;
        if (sourceSet->empty()) {
            WriteFileSyncLog(L"folderAddSet 为空（mode=1），返回空列表", g_emptyWString, g_emptyWString, 5, 3, 2);
            return sortedFolders;
        }
    }
    else if (mode == 2) {
        sourceSet = &qs.folderDeleteSet;
        if (sourceSet->empty()) {
            WriteFileSyncLog(L"folderDeleteSet 为空（mode=2），返回空列表", g_emptyWString, g_emptyWString, 5, 3, 2);
            return sortedFolders;
        }
    }
    else {
        WriteFileSyncLog(L"GetSortedFolderListFromQueueSet mode 参数非法（仅支持1或2）", g_emptyWString, g_emptyWString, 5, 2, 4);
        return sortedFolders;
    }

    try {
        // 提取所有 QueueRow 对象到 vector
        std::vector<QueueRow> rows(sourceSet->begin(), sourceSet->end());

        // 排序规则：先按层级深度，再按 objFullPath 字典序
        std::sort(rows.begin(), rows.end(), [mode](const QueueRow& a, const QueueRow& b) {
            if (a.objFullPath.empty() || b.objFullPath.empty()) {
                return a.objFullPath.empty() ? false : true;  // 空路径放后（防御）
            }

            auto depthA = std::count(a.objFullPath.begin(), a.objFullPath.end(), L'\\');
            auto depthB = std::count(b.objFullPath.begin(), b.objFullPath.end(), L'\\');

            if (depthA != depthB) {
                // mode=1（新增）：浅 → 深
                // mode=2（删除）：深 → 浅
                return (mode == 1) ? (depthA < depthB) : (depthA > depthB);
            }
            return a.objFullPath < b.objFullPath;  // 同层按路径名称升序
            });

        sortedFolders = std::move(rows);

        WriteFileSyncLog(L"获取排序文件夹 QueueRow 列表完成（mode=" + std::to_wstring(mode) +
            L"），共 " + std::to_wstring(sortedFolders.size()) + L" 个目录", g_emptyWString, g_emptyWString, 5, 3, 1);
    }
    catch (...) {
        WriteError(L"GetSortedFolderListFromQueueSet 排序过程中发生异常", g_emptyWString, 4);
        sortedFolders.clear();
    }
    return sortedFolders;
}

// 从 QueueSet 的 otherSet中提取所有 QueueRow 并排序
// mode: 1 = 升序（时间早 → 晚），2 = 降序（时间晚 → 早）
// 排序优先级：timestamp → eventId → action（按 matchGroup=4 规则）→ objFullPath
std::vector<QueueRow> GetSortedQueueListFromQueueSet(const QueueSet& qs, int mode = 1)
{
    std::vector<QueueRow> sortedList;

    // 合并 fileOtherSet 和 folderOtherSet
    std::vector<QueueRow> combined;
    combined.insert(combined.end(), qs.otherSet.begin(), qs.otherSet.end());

    if (combined.empty()) {
        WriteFileSyncLog(L"otherSet 集合为空，返回空列表", g_emptyWString, g_emptyWString, 5, 3, 2);
        return sortedList;
    }

    try {
        // 排序逻辑
        std::sort(combined.begin(), combined.end(), [mode](const QueueRow& a, const QueueRow& b) {
            // 1. timestamp 比较（字符串格式 "YYYY-MM-DD HH:MM:SS"，字典序即时间序）
            if (a.timestamp != b.timestamp) {
                if (mode == 1) {  // 升序：早 → 晚
                    return a.timestamp < b.timestamp;
                }
                else {          // 降序：晚 → 早
                    return a.timestamp > b.timestamp;
                }
            }

            // 2. eventId 比较（数值越大越新）
            try {
                long long idA = a.eventId.empty() ? 0 : std::stoll(a.eventId);
                long long idB = b.eventId.empty() ? 0 : std::stoll(b.eventId);
                if (idA != idB) {
                    if (mode == 1) {
                        return idA < idB;
                    }
                    else {
                        return idA > idB;
                    }
                }
            }
            catch (...) {
                // eventId 解析失败，按字符串比较（防御）
                if (a.eventId != b.eventId) {
                    if (mode == 1) return a.eventId < b.eventId;
                    else return a.eventId > b.eventId;
                }
            }

            // 3. action 比较（使用 matchGroup=4 规则）
            int orderA = GetActionOrder(a.action, 4);
            int orderB = GetActionOrder(b.action, 4);
            if (orderA != orderB) {
                return orderA < orderB;  // 优先级越小越靠前（无论升降序都一致）
            }

            // 4. 兜底：objFullPath 字典序（稳定排序）
            return a.objFullPath < b.objFullPath;
            });

        sortedList = std::move(combined);

        WriteFileSyncLog(L"获取排序 QueueRow 列表完成（otherSet，mode=" + std::to_wstring(mode) +
            L"），共 " + std::to_wstring(sortedList.size()) + L" 条记录", g_emptyWString, g_emptyWString, 5, 3, 1);
    }
    catch (...) {
        WriteError(L"GetSortedQueueListFromQueueSet 排序过程中发生异常", g_emptyWString, 4);
        sortedList.clear();
    }

    return sortedList;
}



// 从原始行集合解析成分类的 QueueSet
QueueSet ParseQueueSet(const std::set<std::wstring>& inputSet)
{
    QueueSet qs;

    if (inputSet.empty()) {
        WriteFileSyncLog(L"输入行集合为空，返回空 QueueSet", g_emptyWString, g_emptyWString, 5, 3, 2);
        return qs;
    }

    try {
        for (const auto& line : inputSet) {
            if (line.empty()) continue;

            QueueRow row = ParseQueueLine(line);
            if (row.objFullPath.empty()) {
                WriteFileSyncLog(L"解析行后 objFullPath 为空，跳过", g_emptyWString, g_emptyWString, 5, 2, 3);
                continue;
            }

            // 判断是否为新增操作（ADD 或 ADDR）
            bool isAdd = (row.action == actionADD || row.action == actionADDR);
            // 判断是否为删除操作（DEL 或 DELR）
            bool isDelete = (row.action == actionDEL || row.action == actionDELR);

            if (row.objType == typeFOLDER) {
                // 文件夹：按 action 分类插入
                if (isAdd) {
                    qs.folderAddSet.insert(row);
                }
                else if (isDelete) {
                    qs.folderDeleteSet.insert(row);
                }
                else {
                    qs.otherSet.insert(row);
                }
            }
            else if (row.objType == typeFILE) {
                // 文件：按 action 分类插入
                if (isAdd) {
                    qs.fileAddSet.insert(row);
                }
                else if (isDelete) {
                    qs.fileDeleteSet.insert(row);
                }
                else {
                    qs.otherSet.insert(row);
                }
            }
            else {
                WriteFileSyncLog(L"未知 对象类型，跳过插入", row.objFullPath, g_emptyWString, 5, 2, 3);
            }
        }

        WriteFileSyncLog(L"ParseQueueSet 完成解析，"
            L"folderAdd:" + std::to_wstring(qs.folderAddSet.size()) +
            L"，folderDelete:" + std::to_wstring(qs.folderDeleteSet.size()) +
            L"，fileAdd:" + std::to_wstring(qs.fileAddSet.size()) +
            L"，fileDelete:" + std::to_wstring(qs.fileDeleteSet.size()) +
            L"，other:" + std::to_wstring(qs.otherSet.size()),
            g_emptyWString, g_emptyWString, 5, 3, 1);
    }
    catch (...) {
        WriteError(L"ParseQueueSet 解析过程中发生异常", g_emptyWString, 4);
        qs.folderAddSet.clear();
        qs.folderDeleteSet.clear();
        qs.otherSet.clear();
        qs.fileAddSet.clear();
        qs.fileDeleteSet.clear();

    }

    return qs;
}


// 查找方法：从 QueueSet 中按 eventId + action 匹配找到第一个 QueueRow立即返回，如果有多个匹配则该方法不合适
QueueRow FindQueueRowFromQueueSetByEventAAction(
    const std::set<QueueRow>& subset, const std::wstring& eventId, const std::set<std::wstring>& targetActions)
{
    QueueRow emptyRow;  // 默认空返回

    if (eventId.empty() || targetActions.empty()) {
        return emptyRow;
    }

    // 使用 const 指针避免拷贝整个 set（性能优化，几万条数据时关键）
    const std::set<QueueRow>* pTargetSet = nullptr;

    // 使用 switch 选择目标子 set
    if (subset.empty()) {
        return emptyRow;
    }
    try {
        // 直接遍历目标 set，查找匹配条件
        for (const auto& row : subset) {
            if (row.eventId == eventId && targetActions.count(row.action) > 0) {
                return row;  // 找到第一个匹配的记录，立刻返回
            }
        }
        // 未找到匹配项
        return emptyRow;
    }
    catch (...) {
        WriteError(L"FindQueueRowFromQueueSetByEventAAction 查找过程中发生异常", g_emptyWString, 4);
        return emptyRow;
    }
}


// 从指定的 std::set<QueueRow> 中，按 objFullPath + action 匹配查找第一个匹配的 QueueRow（只查不删）
// 用于跨 eventId 的关联查询，例如判断某个新增记录是否有对应的重命名或删除记录
QueueRow FindQueueRowFromQueueSetByFullPathAAction(
    const std::set<QueueRow>& subset, const std::wstring& fullPath, const std::set<std::wstring>& targetActions)
{
    QueueRow emptyRow;
    if (fullPath.empty() || targetActions.empty() || subset.empty()) {
        return emptyRow;
    }
    try {
        for (const auto& row : subset) {
            if (row.objFullPath == fullPath && targetActions.count(row.action) > 0) {
                return row;
            }
        }
        return emptyRow;
    }
    catch (...) {
        WriteError(L"FindQueueRowFromQueueSetByFullPathAAction 查找过程中发生异常", g_emptyWString, 4);
        return emptyRow;
    }
}

// 从指定的 std::set<QueueRow> 中，按 eventId + action 匹配删除第一个匹配的 QueueRow
// 如果找到并删除，返回 true；否则返回 false（未找到或异常）
// 在大量循环中非常实用，如果用match返回一次，再void删除一次，实际上是两次循环，非常耗性能，尤其在循环中循环时。这个方法循环一次，既匹配又删除
QueueRow DeleteQueueRowFromQueueSetByEventAAction(
    std::set<QueueRow>& subset,                     // 注意：非 const，必须可修改
    const std::wstring& eventId,
    const std::set<std::wstring>& targetActions)
{
    QueueRow result;
    if (eventId.empty() || targetActions.empty() || subset.empty()) {
        return result;
    }

    try {
        // 直接遍历 set，找到第一个匹配的记录并删除
        for (auto it = subset.begin(); it != subset.end(); ++it) {
            if (it->eventId == eventId && targetActions.count(it->action) > 0) {
                result = *it;
                subset.erase(it);  // 安全删除（erase 返回下一个迭代器，但我们立即返回）
                return result;       // 成功删除
            }
        }

        // 未找到匹配项
        return result;
    }
    catch (...) {
        WriteError(L"DeleteQueueRowFromQueueSetByEventAAction 删除过程中发生异常", g_emptyWString, 4);
        return result;
    }
}

// 从指定的 std::set<QueueRow> 中，按 objName、时间戳、动作类型匹配删除第一个匹配的 QueueRow，且绝对路径不能相同
// 如果找到并删除，返回 true；否则返回 false（未找到或异常）。给文件、文件夹剪切移动场景使用
QueueRow DeleteQueueRowFromQueueSetByNameAAction(
    std::set<QueueRow>& subset,                     // 注意：非 const，必须可修改
    const std::wstring& objName,
    const std::wstring& timestamp,
    const std::wstring& objFullPath,
    const std::set<std::wstring>& targetActions)
{
    QueueRow result;
    if (objName.empty() || targetActions.empty() || subset.empty() || timestamp.empty() || objFullPath.empty()) {
        return result;
    }

    try {
        // 直接遍历 set，找到第一个匹配的记录并删除
        for (auto it = subset.begin(); it != subset.end(); ++it) {
            if (it->objName == objName && targetActions.count(it->action) > 0 && almostSameTime(timestamp, it->timestamp, 2) && it->objFullPath != objFullPath) {
                result = *it;
                subset.erase(it);  // 安全删除（erase 返回下一个迭代器，但我们立即返回）
                return result;       // 成功删除
            }
        }
        // 未找到匹配项
        return result;
    }
    catch (...) {
        WriteError(L"DeleteQueueRowFromQueueSetByNameAAction 删除过程中发生异常", g_emptyWString, 4);
        return result;
    }
}


// 优化 QueueSet：检测递归目录树，删除子记录（含目录和文件），只保留根，
// 然后再结合增删目录操作时的“递归”操作，完成对用户行为的还原
// 优化 QueueSet：检测递归目录树，删除子记录（含目录和文件），只保留根，
// 然后再结合增删目录操作时的“递归”操作，完成对用户行为的还原
void RemoveRecursiveFoldersForQueueSet(QueueSet& qs, int mode)
{
    std::set<QueueRow>* targetFolderSet = nullptr;
    std::set<QueueRow>* targetFileSet = nullptr;
    std::wstring logType;

    if (mode == 1) {
        targetFolderSet = &qs.folderAddSet;
        targetFileSet = &qs.fileAddSet;
        logType = L"新增目录";
    }
    else if (mode == 2) {
        targetFolderSet = &qs.folderDeleteSet;
        targetFileSet = &qs.fileDeleteSet;
        logType = L"删除目录";
    }
    else {
        WriteFileSyncLog(L"RemoveRecursiveFoldersForQueueSet mode 非法（仅1或2）", g_emptyWString, g_emptyWString, 5, 2, 4);
        return;
    }

    if (targetFolderSet->empty()) {
        WriteFileSyncLog(L"目标 folder set 为空，无需优化（mode=" + std::to_wstring(mode) + L"）", g_emptyWString, g_emptyWString, 5, 3, 2);
        return;
    }

    try {
        // 步骤1: 提取所有文件夹路径到 unordered_set（快速查找），并标准化路径（无尾斜杠）
        std::unordered_set<std::wstring> allFolderPaths;
        for (const auto& row : *targetFolderSet) {
            if (!row.objFullPath.empty()) {
                std::wstring normalized = FormatPath(row.objFullPath);  // 假设 FormatPath 去除尾斜杠
                allFolderPaths.insert(normalized);
            }
        }

        // 步骤2: 建树：unordered_map<parent, vector<child>>
        std::unordered_map<std::wstring, std::vector<std::wstring>> parentToChildren;
        for (const auto& path : allFolderPaths) {
            std::wstring parent = GetParentPathNoBackSlash(path);
            if (!parent.empty()) {
                parentToChildren[parent].push_back(path);
            }
        }

        // 步骤3: 找出所有根（其父不在 allFolderPaths 中）
        std::vector<std::wstring> roots;
        for (const auto& path : allFolderPaths) {
            std::wstring parent = GetParentPathNoBackSlash(path);
            if (parent.empty() || allFolderPaths.find(parent) == allFolderPaths.end()) {
                roots.push_back(path);
            }
        }

        // 步骤4: 对于每个根，DFS 检查其完整子树是否都在 allFolderPaths 中
        size_t totalRemovedFolders = 0;
        size_t totalRemovedFiles = 0;
        for (const auto& root : roots) {
            // DFS 收集子树所有路径（包括根）
            std::vector<std::wstring> subtree;
            std::function<void(const std::wstring&)> dfs = [&](const std::wstring& cur) {
                subtree.push_back(cur);
                auto it = parentToChildren.find(cur);
                if (it != parentToChildren.end()) {
                    for (const auto& child : it->second) {
                        dfs(child);
                    }
                }
                };
            dfs(root);

            // 检查子树完整性：子树大小 >1（有子项），且所有子项都在 allFolderPaths（假设完整递归）
            if (subtree.size() > 1) {
                bool isComplete = true;
                for (const auto& sub : subtree) {
                    if (allFolderPaths.find(sub) == allFolderPaths.end()) {
                        isComplete = false;
                        break;
                    }
                }

                if (isComplete) {
                    // 是完整递归树：删除所有子孙记录，只保留根
                    for (size_t i = 1; i < subtree.size(); ++i) {  // 从1开始，跳过根
                        QueueRow dummy;
                        dummy.objFullPath = subtree[i];
                        targetFolderSet->erase(dummy);
                        ++totalRemovedFolders;
                    }

                    // ==================== 修正：同步删除文件 set 中的子树相关文件 ====================
                    // 使用前缀匹配：文件父目录是否以子树中任意路径开头（包括根）
                    for (auto fileIt = targetFileSet->begin(); fileIt != targetFileSet->end(); ) {
                        std::wstring fileParent = GetParentPathNoBackSlash(fileIt->objFullPath);

                        bool belongsToSubtree = false;
                        for (const auto& subPath : subtree) {  // 包括根
                            // 标准化 subPath 和 fileParent，确保无尾斜杠
                            std::wstring normSub = FormatPath(subPath);
                            std::wstring normParent = FormatPath(fileParent);

                            // ==================== 新增日志：调试匹配 ====================
                            std::wstring logMsg = L"检查文件父路径：" + normParent + L" 是否属于子树路径：" + normSub;
                            WriteFileSyncLog(logMsg, g_emptyWString, g_emptyWString, 5, 3, 1);  // 信息级日志
                            // ================================================

                            if (ContainSubFolderOrEqual(normParent, normSub)) {
                                belongsToSubtree = true;
                                break;
                            }
                        }

                        if (belongsToSubtree) {
                            fileIt = targetFileSet->erase(fileIt);  // 删除并推进迭代器
                            ++totalRemovedFiles;
                        }
                        else {
                            ++fileIt;  // 未匹配，继续下一个
                        }
                    }
                    // ================================================
                }
            }
        }

        //WriteFileSyncLog(L"RemoveRecursiveFoldersForQueueSet 完成（" + logType + L"，mode=" + std::to_wstring(mode) +
        //    L"），移除子文件夹记录 " + std::to_wstring(totalRemovedFolders) +
        //    L" 条，移除相关文件记录 " + std::to_wstring(totalRemovedFiles) + L" 条，只剩根", g_emptyWString, g_emptyWString, 5, 3, 1);
    }
    catch (...) {
        WriteError(L"RemoveRecursiveFoldersForQueueSet 优化过程中发生异常", g_emptyWString, 4);
    }
}




// 定义一个queue行类型的字段枚举，用于校验有效的业务记录
enum class QueueField {
    Action = 1,
    ObjType,
    ObjName,
    ObjFullPath,
    Timestamp,
    EventId,
    Status,
    COUNT
};

std::wstring GetQueueFieldValue(const QueueRow& row, QueueField field)
{
    switch (field) {
    case QueueField::Action:      return row.action;
    case QueueField::ObjType:     return row.objType;
    case QueueField::ObjName:     return row.objName;
    case QueueField::ObjFullPath: return row.objFullPath;
    case QueueField::Timestamp:   return row.timestamp;
    case QueueField::EventId:     return row.eventId;
    case QueueField::Status:      return row.status;
    default:                      return g_emptyWString;
    }
}

struct QueueValidateResult {
    bool valid = true;
    std::wstring error;   // 失败原因
    int fieldIndex = -1;  // 出错字段（1-based）
    wstring eventId = g_emptyWString;
};

QueueValidateResult ValidateQueueLine(const std::wstring& iline)
{
    QueueValidateResult result;

    std::wstring line = FormattedLine(iline);


    // ===== 1. 空行 / 注释行 =====
    if (line.empty() ||
        line[0] == L'#' ||
        (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) {
        result.valid = false;
        result.error = L"空行或注释行";
        return result;
    }

    // ===== 1.1 快速 Action 校验 =====
    if (line.empty() || line[0] != g_delimiter) {
        result.valid = false;
        result.error = L"坏记录，格式错误";
        return result;
    }
    else {
        // 去掉首个双引号
        std::wstring remaining = line.substr(1);
        bool matched = false;

        if (ContainStringOrEqual(remaining, actionADD) ||
            ContainStringOrEqual(remaining, actionADDR) ||
            ContainStringOrEqual(remaining, actionMOD) ||
            ContainStringOrEqual(remaining, actionDEL) ||
            ContainStringOrEqual(remaining, actionDELR) ||
            ContainStringOrEqual(remaining, actionRENOLD) ||
            ContainStringOrEqual(remaining, actionRENNEW) ||
            ContainStringOrEqual(remaining, actionRENOLDR) ||
            ContainStringOrEqual(remaining, actionRENNEWR) ||
            ContainStringOrEqual(remaining, actionUNKNOWN))
        {
            matched = true;
        }

        if (!matched) {
            result.valid = false;
            result.error = L"坏记录，格式错误（操作类型不合法）";
            return result;
        }
    }

    // ===== 2. 双引号数量必须成对 =====
    size_t quoteCount = std::count(line.begin(), line.end(), g_delimiter);
    if (quoteCount == 0 || quoteCount % 2 != 0) {
        result.valid = false;
        result.error = L"双引号分割符不成对";
        return result;
    }

    // ===== 2.a 双引号数量必须成对 =====
    size_t qCount = std::count(line.begin(), line.end(), g_delimiter);
    if (qCount != 14) {
        result.valid = false;
        result.error = L"坏记录，双引号分割符数量不正确";
        return result;
    }

    // ===== 3. 解析字段 =====
    auto row = ParseQueueLine(line);

    // ===== 4. 字段数量校验 =====
    //if (row.fieldCount != static_cast<int>(QueueField::COUNT) - 1) {
    //    result.valid = false;
    //    result.error = L"字段数量不正确";
    //    return result;
    //}

    // ===== 5. 字段级约束 =====


    // ===== 坏记录（日志损坏/拼接）检测 =====
    // 经验规则：损坏行通常会导致以下字段异常短或包含明显不合理的残缺内容
    if (row.action.size() < 3 ||                     // 正常 action 如 ADDED/MODIFIED/DELETED/RENAMED，至少 5 个字符，最短也至少 3（理论上）
        row.objType.size() < 4 ||                    // FILE 或 FOLDER，至少 4 个字符
        row.objName.empty() || row.objName.size() > 512 ||  // 文件/文件夹名不能为空，且过长也不合理
        row.eventId.size() < 4 || row.eventId.size() > 20)   // eventId 通常是 7~8 位数字
    {
        result.valid = false;
        result.error = L"疑似日志行损坏（字段异常，可能为多行拼接或截断）";
        return result;
    }
    // 如果 action 字段中包含反斜杠 \ ，极大概率是路径残片污染
    if (row.action.find(L'\\') != std::wstring::npos ||
        row.action.find(L'/') != std::wstring::npos)
    {
        result.valid = false;
        result.error = L"操作类型字段包含路径分隔符，疑似日志损坏";
        return result;
    }


    // Action
    if (row.action.empty()) {
        result.valid = false;
        result.fieldIndex = (int)QueueField::Action;
        result.error = L"操作类型为空";
        result.eventId = row.eventId;
        return result;
    }

    // ObjType
    if (row.objType != typeFILE && row.objType != typeFOLDER) {
        result.valid = false;
        result.fieldIndex = (int)QueueField::ObjType;
        result.error = L"对象类型值非法";
        result.eventId = row.eventId;
        return result;
    }

    // Status
    std::wstring s = row.status;
    if (!(s == aStatusINIT || s == aStatusMSUCCESS || s == aStatusMBAD || s == aStatusMFAILED || s == aStatusMCORRECTED || s == aStatusMUNKNOWN || s == aStatusMEXCLUDED
        || s == aStatusPSUCCESS || s == aStatusPRETRY || s == aStatusPFAILED
        || s == aStatusMREMOVED || s == aStatusMMERGED || ContainStringOrEqual(s, aStatusMRETRY) || s.empty())) {
        result.valid = false;
        result.fieldIndex = (int)QueueField::Status;
        result.error = L"状态值非法";
        result.eventId = row.eventId;
        return result;
    }

    // FullPath
    //if (row.objFullPath.find(L"\\") == std::wstring::npos) {
    //    result.valid = false;
    //    result.fieldIndex = (int)QueueField::ObjFullPath;
    //    result.error = L"fullPath 非法";
    //    return result;
    //}

    // 去除首尾空格（可选）
    std::wstring fullPath = row.objFullPath;
    fullPath.erase(0, fullPath.find_first_not_of(L" \t\r\n"));
    fullPath.erase(fullPath.find_last_not_of(L" \t\r\n") + 1);

    // 严格校验本地盘符路径 或 UNC 路径
    // 如 C盘盘符
    if (!((fullPath.size() >= 3 && fullPath[1] == L':' && fullPath[2] == L'\\') ||
        (fullPath.size() >= 2 && fullPath.substr(0, 2) == L"\\\\")))
    {
        result.valid = false;
        result.fieldIndex = (int)QueueField::ObjFullPath;
        result.error = L"完整路径必须为 Windows 绝对路径（本地盘符或 UNC 网络路径）";
        result.eventId = row.eventId;
        return result;
    }

    // Timestamp
    std::tm tm = {};
    std::wistringstream ss(row.timestamp);
    ss >> std::get_time(&tm, L"%Y-%m-%d %H:%M:%S");
    if (ss.fail()) {
        result.valid = false;
        result.fieldIndex = (int)QueueField::Timestamp;
        result.error = L"时间戳格式非法";
        result.eventId = row.eventId;
        return result;
    }

    // EventId
    if (row.eventId.empty()) {
        result.valid = false;
        result.fieldIndex = (int)QueueField::EventId;
        result.error = L"Event ID为空";
        result.eventId = row.eventId;
        return result;
    }

    return result;
}



// 从记录集找符合条件的queue记录，所有条件都可用于查询，都支持通配，匹配的记录集内排序（时间戳 → eventId → action），统一升序或降序
std::vector<std::wstring> FindQueueLines(
    const std::vector<std::wstring> inputLines,
    const std::wstring eventId,
    const std::wstring action,
    const std::wstring objType,
    const std::wstring fullName,
    const std::wstring status,
    const std::wstring fullPath,
    const std::wstring timestmp,
    const std::wstring sortOrder)
{
    std::vector<std::wstring> matchedLines;

    // 1. 严格校验必填入参 (入参 2-6 不能为空，且入参 2 不允许通配符)
    if (eventId.empty() || action.empty() || objType.empty() || fullName.empty() || fullPath.empty()) {
        std::wstring wmsg = L"FindQueueLine入参调用非法，可通配";
        WriteMergeLog(4, wmsg, g_emptyWString, 3);
        return {};
    }

    // 2. 检查文件是否存在
    if (inputLines.empty()) {
        std::wstring wmsg = L"FindQueueLine结果集不能为空";
        WriteMergeLog(4, wmsg, g_emptyWString, 3);
        return {};
    }

    // 4. 逐行匹配
    try {
        for (const auto& line : inputLines) {
            if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;


            // 获取各列值用于比对
            auto row = ParseQueueLine(line);
            auto curAction = row.action;
            auto curObjType = row.objType;
            auto curObjName = row.objName;
            auto curEventId = row.eventId;
            auto curStatus = row.status;
            auto curFullPath = row.objFullPath;
            auto timestamp = row.timestamp;
            if (curStatus.find_first_not_of(L" ") == std::wstring::npos) curStatus = g_emptyWString;


            // 匹配逻辑：如果是通配符则忽略比对，否则必须完全相等
            bool isMatch = true;
            if (isMatch && eventId != L"*" && curEventId != eventId) isMatch = false;
            if (isMatch && action != L"*" && curAction != action)   isMatch = false;
            if (isMatch && objType != L"*" && curObjType != objType)  isMatch = false;
            if (isMatch && fullName != L"*" && curObjName != fullName) isMatch = false;
            if (isMatch && fullPath != L"*" && curFullPath != fullPath) isMatch = false;
            if (isMatch && timestmp != L"*" && timestamp != timestmp) isMatch = false;
            if (isMatch && status != L"*" && curStatus != status)   isMatch = false;

            if (isMatch) {
                matchedLines.push_back(line);
            }
        }
    }
    catch (const std::exception& e) {
        std::wstring wmsg = L"文件处理发生异常，方法FindQueueLine，" + ToWide(e.what());
        WriteMergeLog(4, wmsg, g_emptyWString, 3);
    }

    if (matchedLines.empty()) return {};

    // 5. 多字段排序逻辑：时间戳 + eventId + action
    auto parseTime = [](const std::wstring& lineText) -> std::time_t {
        std::wstring tsStr = ReadLineElementByIndex(lineText, 5, g_delimiter);
        if (tsStr.empty()) return 0;

        std::tm tm = {};
        std::wistringstream ss(tsStr);
        // 格式：2025-12-22 17:03:25
        ss >> std::get_time(&tm, L"%Y-%m-%d %H:%M:%S");
        if (ss.fail()) return 0;
        return std::mktime(&tm);
        };

    // 6. 执行排序（时间戳 → eventId → action），按入参升降序统一控制
    std::sort(matchedLines.begin(), matchedLines.end(),
        [&](const std::wstring& a, const std::wstring& b) {

            auto rowA = ParseQueueLine(a);
            auto rowB = ParseQueueLine(b);

            std::time_t tA = parseTime(a);
            std::time_t tB = parseTime(b);

            if (tA != tB) {
                return (sortOrder == L"DESC") ? (tA > tB) : (tA < tB);
            }

            if (rowA.eventId != rowB.eventId) {
                return (sortOrder == L"DESC")
                    ? (rowA.eventId > rowB.eventId)
                    : (rowA.eventId < rowB.eventId);
            }

            if (rowA.action != rowB.action) {
                return (sortOrder == L"DESC")
                    ? (rowA.action > rowB.action)
                    : (rowA.action < rowB.action);
            }

            return false;
        });

    return matchedLines;
}



// 从文件查找符合条件的queue记录
std::vector<std::wstring> FindQueueLinesFromFile(
    const std::wstring filePath,
    const std::wstring eventId,
    const std::wstring action,
    const std::wstring objType,
    const std::wstring fullName,
    const std::wstring status,
    const std::wstring sortOrder)
{
    std::vector<std::wstring> matchedLines;

    // 1. 严格校验必填入参 (入参 2-6 不能为空，且入参 2 不允许通配符)
    if (eventId.empty() || eventId == L"*" || action.empty() || objType.empty() || fullName.empty()) {
        std::wstring wmsg = L"FindQueueLine入参调用非法，Event ID必填，其它字段至少“*”通配";
        WriteMergeLog(4, wmsg, g_emptyWString, 3);
        return {};
    }

    // 2. 检查文件是否存在
    if (!std::filesystem::exists(filePath)) {
        // 注意：将 wstring 转换为 string 打印日志，此处建议使用之前讨论的转换方法
        std::wstring wmsg = L"FindQueueLine原文件" + filePath + L"不存在";
        WriteMergeLog(4, wmsg, g_emptyWString, 3);
        return {};
    }

    // ===================== 文件级互斥锁（打开文件前加锁） =====================
    std::lock_guard<std::mutex> lock(g_queueMutex);

    // 3. 使用 ifs + 统一文件读取方法
    std::vector<std::wstring> fileLines;
    try {
        fileLines = FileToWStringLinesXY(filePath);
    }
    catch (const std::exception& e) {
        std::wstring wmsg = L"FindQueueLine读取文件发生异常：" + filePath + L"，" + ToWide(e.what());
        WriteMergeLog(4, wmsg, g_emptyWString, 3);
        return {};
    }

    // 4. 逐行匹配
    try {
        for (const auto& line : fileLines) {
            if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;


            // 获取各列值用于比对
            auto row = ParseQueueLine(line);
            auto curAction = row.action;
            auto curObjType = row.objType;
            auto curObjName = row.objName;
            auto curEventId = row.eventId;
            auto curStatus = row.status;
            if (curStatus.find_first_not_of(L" ") == std::wstring::npos) curStatus = g_emptyWString;


            // 匹配逻辑：如果是通配符则忽略比对，否则必须完全相等
            bool isMatch = true;
            if (curEventId != eventId) isMatch = false;
            if (isMatch && action != L"*" && curAction != action)   isMatch = false;
            if (isMatch && objType != L"*" && curObjType != objType)  isMatch = false;
            if (isMatch && fullName != L"*" && curObjName != fullName) isMatch = false;
            if (isMatch && status != L"*" && curStatus != status)   isMatch = false;

            if (isMatch) {
                matchedLines.push_back(line);
            }
        }
    }
    catch (const std::exception& e) {
        std::wstring wmsg = L"文件处理发生异常，方法FindQueueLine，" + ToWide(e.what());
        WriteMergeLog(4, wmsg, g_emptyWString, 3);
    }

    if (matchedLines.empty()) return {};

    // 5. 多字段排序逻辑：时间戳 + eventId + action
    auto parseTime = [](const std::wstring& lineText) -> std::time_t {
        std::wstring tsStr = ReadLineElementByIndex(lineText, 5, g_delimiter);
        if (tsStr.empty()) return 0;

        std::tm tm = {};
        std::wistringstream ss(tsStr);
        // 格式：2025-12-22 17:03:25
        ss >> std::get_time(&tm, L"%Y-%m-%d %H:%M:%S");
        if (ss.fail()) return 0;
        return std::mktime(&tm);
        };

    // 6. 执行排序（时间戳 → eventId → action），按入参升降序统一控制
    std::sort(matchedLines.begin(), matchedLines.end(),
        [&](const std::wstring& a, const std::wstring& b) {

            auto rowA = ParseQueueLine(a);
            auto rowB = ParseQueueLine(b);

            std::time_t tA = parseTime(a);
            std::time_t tB = parseTime(b);

            if (tA != tB) {
                return (sortOrder == L"DESC") ? (tA > tB) : (tA < tB);
            }

            if (rowA.eventId != rowB.eventId) {
                return (sortOrder == L"DESC")
                    ? (rowA.eventId > rowB.eventId)
                    : (rowA.eventId < rowB.eventId);
            }

            if (rowA.action != rowB.action) {
                return (sortOrder == L"DESC")
                    ? (rowA.action > rowB.action)
                    : (rowA.action < rowB.action);
            }

            return false;
        });

    return matchedLines;
}






// 替换行文本中指定索引（从1开始）的 element 内容，仅替换引号内文本，其它字符保持不变
std::wstring UpdateLineElementByIndex(const std::wstring lineText, int index, const std::wstring& newValue) {
    try {
        // ===== 基本校验 =====
        if (index <= 0) {
            std::wstring wmsg = L"UpdateLineElementByIndex 参数异常，index 非法：" + std::to_wstring(index);
            WriteError(wmsg, lineText, 4);
            return g_emptyWString;
        }

        // newValue 不允许包含双引号（否则会破坏结构）
        if (newValue.find(L'\"') != std::wstring::npos) {
            std::wstring wmsg = L"UpdateLineElementByIndex 参数异常，设置值包含双引号：" + newValue;
            WriteError(wmsg, lineText, 4);
            return g_emptyWString;
        }

        // 校验双引号是否成对
        size_t quoteCount = std::count(lineText.begin(), lineText.end(), L'\"');
        if (quoteCount == 0 || quoteCount % 2 != 0) {
            std::wstring wmsg = L"UpdateLineElementByIndex 解析异常，双引号不成对，替换前文本：" + lineText;
            WriteError(wmsg, lineText, 4);
            return g_emptyWString;
        }

        // 校验是否包含换行符
        if (lineText.find_first_of(L"\r") != std::wstring::npos || lineText.find_first_of(L"\n") != std::wstring::npos) {
            std::wstring wmsg = L"UpdateLineElementByIndex 解析异常，文本包含换行符";
            WriteError(wmsg, g_emptyWString, 4);
            return g_emptyWString;
        }

        // ===== 解析 element 区间（
        struct ElementPos {
            size_t contentStart; // 不含 "
            size_t contentEnd;   // 不含 "
        };

        std::vector<ElementPos> elements;
        bool inQuotes = false;
        size_t currentStart = 0;

        for (size_t i = 0; i < lineText.length(); ++i) {
            wchar_t c = lineText[i];
            if (c == L'\"') {
                if (!inQuotes) {
                    // 开始一组双引号
                    inQuotes = true;
                    currentStart = i + 1;
                }
                else {
                    // 结束一组双引号
                    elements.push_back({ currentStart, i });
                    inQuotes = false;
                }
            }
        }

        if (index > (int)elements.size()) {
            std::wstring wmsg = L"UpdateLineElementByIndex index 超出元素数量，index 非法：" + std::to_wstring(index);
            WriteError(wmsg, lineText, 4);
            return g_emptyWString;
        }

        // ===== 构造新行文本 =====
        const ElementPos& target = elements[index - 1];

        std::wstring newLine;
        newLine.reserve(lineText.size() + newValue.size());

        // 前半部分（目标内容之前）
        newLine.append(lineText.substr(0, target.contentStart));

        // 替换内容
        newLine.append(newValue);

        // 后半部分（目标内容之后）
        newLine.append(lineText.substr(target.contentEnd));

        return newLine;
    }
    catch (...) {
        std::wstring wmsg = L"更新数据行发生异常";
        WriteError(wmsg, lineText, 4);
        return g_emptyWString;
    }
}

// 在指定索引位置插入一个新的数据字段（index 从 1 开始）
std::wstring InsertLineElementByIndex(const std::wstring lineText, int index, const std::wstring& newValue) {
    try {
        // ===== 基本校验 =====
        if (index <= 0) {
            WriteError(L"InsertLineElementByIndex 参数异常，index 非法", lineText, 4);
            return g_emptyWString;
        }

        if (newValue.find(L'\"') != std::wstring::npos) {
            WriteError(L"InsertLineElementByIndex 参数异常，newValue 包含双引号", lineText, 4);
            return g_emptyWString;
        }

        size_t quoteCount = std::count(lineText.begin(), lineText.end(), L'\"');
        if (quoteCount % 2 != 0) {
            WriteError(L"InsertLineElementByIndex 解析异常，双引号不成对", lineText, 4);
            return g_emptyWString;
        }

        // ===== 解析所有元素的位置 =====
        struct ElementPos {
            size_t fullStart; // 包含左侧引号
            size_t fullEnd;   // 包含右侧引号
        };
        std::vector<ElementPos> elements;
        bool inQuotes = false;
        size_t startPos = 0;

        for (size_t i = 0; i < lineText.length(); ++i) {
            if (lineText[i] == L'\"') {
                if (!inQuotes) {
                    inQuotes = true;
                    startPos = i;
                }
                else {
                    elements.push_back({ startPos, i });
                    inQuotes = false;
                }
            }
        }

        int currentCount = (int)elements.size();
        // 允许插入的位置是从 1 到 currentCount + 1 (即末尾追加)
        if (index > currentCount + 1) {
            WriteError(L"InsertLineElementByIndex index 超出范围", lineText, 4);
            return g_emptyWString;
        }

        // ===== 构造新行 =====
        std::wstring newLine;
        std::wstring formattedValue = L"\"" + newValue + L"\"";
        std::wstring separator = L"    "; // 4个空格

        if (index == 1) {
            // 场景 A：插入到最前面
            if (currentCount == 0) {
                newLine = formattedValue;
            }
            else {
                // 新字段 + 4空格 + 原有整行
                newLine = formattedValue + separator + lineText;
            }
        }
        else if (index == currentCount + 1) {
            // 场景 B：追加到最后面
            newLine = lineText + separator + formattedValue;
        }
        else {
            // 场景 C：插入到中间
            // 插入位置是第 index 个元素的起始引号处
            size_t splitPos = elements[index - 1].fullStart;
            // 前半部分 + 新字段 + 4空格 + 后半部分
            newLine = lineText.substr(0, splitPos) + formattedValue + separator + lineText.substr(splitPos);
        }

        return newLine;
    }
    catch (...) {
        WriteError(L"插入数据行发生异常", lineText, 4);
        return g_emptyWString;
    }
}

// 将指定位置 index 的字段移动到新位置 newIndex（所有位置从1开始算，而非0）
std::wstring MoveLineElementByIndex(const std::wstring lineText, int index, int newIndex) {
    try {
        // 1. 获取所有元素位置，用于验证 index 范围
        std::vector<std::pair<size_t, size_t>> elements; // first:左引号, second:右引号
        bool inQuotes = false;
        size_t startPos = 0;
        for (size_t i = 0; i < lineText.length(); ++i) {
            if (lineText[i] == L'\"') {
                if (!inQuotes) { inQuotes = true; startPos = i; }
                else { elements.push_back({ startPos, i }); inQuotes = false; }
            }
        }

        int count = (int)elements.size();
        if (index <= 0 || index > count || newIndex <= 0 || newIndex > count) {
            WriteError(L"MoveLineElementByIndex 参数索引越界", lineText, 4);
            return lineText; // 异常时返回原行
        }

        if (index == newIndex) return lineText; // 位置相同无需移动

        // 2. 提取第 index 个字段的值（不含引号）
        std::wstring targetValue = lineText.substr(elements[index - 1].first + 1,
            elements[index - 1].second - elements[index - 1].first - 1);

        // 3. 从原行中删除该字段及其邻近的分隔符（4个空格）
        // 确定删除区间的原则：
        // 如果是第一个，删掉它和它后面的空格；否则删掉它和它前面的空格
        size_t eraseStart = elements[index - 1].first;
        size_t eraseEnd = elements[index - 1].second;

        std::wstring tempLine = lineText;
        if (index == 1) {
            // 后面可能有空格，寻找第一个元素后的空格并一并删除
            size_t nextSearch = eraseEnd + 1;
            while (nextSearch < tempLine.length() && tempLine[nextSearch] == L' ') {
                nextSearch++;
            }
            tempLine.erase(eraseStart, nextSearch - eraseStart);
        }
        else {
            // 前面一定有空格，寻找前一个元素结束后的空格并一并删除
            size_t prevSearch = eraseStart - 1;
            while (prevSearch > 0 && tempLine[prevSearch] == L' ') {
                prevSearch--;
            }
            // 注意保留前一个字段的右引号
            size_t actualStart = prevSearch + 1;
            tempLine.erase(actualStart, eraseEnd - actualStart + 1);
        }

        // 4. 调用插入方法到新位置
        // 注意：删除一个元素后，总数减1，InsertLineElementByIndex 会处理 newIndex
        std::wstring result = InsertLineElementByIndex(tempLine, newIndex, targetValue);

        if (result == g_emptyWString) return lineText;
        return result;
    }
    catch (...) {
        WriteError(L"移动数据字段发生异常", lineText, 4);
        return lineText;
    }
}







/**
 * 刪除隊列中匹配的行，並將其備份到指定文件
 * 入參：srcFile(原文件), eventId, actionType, objectName, backupFile(備份文件)
 */
void MoveQueueLinesBetweenFiles(
    const std::wstring& srcFile,
    const std::wstring& eventId,
    const std::wstring& actionType,
    const std::wstring& objectName, // 名稱建議傳入 wstring 以匹配 queue 內的中文
    const std::wstring& backupFile)
{
    // 1. 參數合法性校驗
    if (srcFile.empty() || backupFile.empty() || eventId.empty() || actionType.empty() || objectName.empty()) {
        return;
    }

    try {
        // 使用全局互斥鎖保護文件操作（建議在全局定義此 mutex）
        std::vector<unsigned char> raw;
        {
            // 将 lock 的作用域限制在文件读取和解析阶段
            std::lock_guard<std::mutex> lock(g_queueMutex);
            raw = ReadAllBytes(srcFile);
        }
        if (raw.empty()) return;

        // 轉為 wstring 進行解析
        // std::wstring content = ToWide(std::string(reinterpret_cast<char*>(raw.data()), raw.size()));
        std::wstring content = BytesToWString(raw);
        std::wstringstream wss(content);
        std::wstring line;

        std::vector<std::wstring> remainLines;   // 剩餘的行
        std::vector<std::wstring> matchedLines;  // 匹配(要刪除)的行

        // 3. 逐行解析與匹配
        while (std::getline(wss, line)) {
            if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;

            // 規範化處理（移除換行符及首尾空格）
            FormatLine(line);
            if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;

            // 提取關鍵字段進行比對
            // 根據 queues 格式：Index 1:Action, Index 3:FileName, Index 6:EventId
            auto row = ParseQueueLine(line);
            std::wstring curAction = row.action;
            std::wstring curName = row.objName;
            std::wstring curEvent = row.eventId;

            // 通配符與精確匹配判斷
            bool isActionMatch = (actionType == L"*") || (curAction == actionType);
            bool isNameMatch = (objectName == L"*") || (curName == objectName);
            bool isEventMatch = (eventId == L"*") || (curEvent == eventId);

            if (isActionMatch && isNameMatch && isEventMatch) {
                matchedLines.push_back(line);
            }
            else {
                remainLines.push_back(line);
            }
        }

        // 4. 如果沒有匹配項，直接退出
        if (matchedLines.empty()) return;

        // 5. 將匹配的行添加(Append)到備份文件
        fs::path p(backupFile);
        // 1. 健壮性保护：检查父目录是否存在，不存在则创建
        if (p.has_parent_path() && !fs::exists(p.parent_path())) {
            fs::create_directories(p.parent_path());
        }

        std::ofstream bFile(backupFile, std::ios::app | std::ios::binary);
        if (bFile.is_open()) {
            for (const auto& l : matchedLines) {
                std::string utf8Line = ToUTF8(l) + "\r\n";
                bFile.write(utf8Line.c_str(), utf8Line.size());
            }
            bFile.flush();
            bFile.close();
        }

        // 6. 將剩餘的行覆蓋(Trunc)寫回原文件
        std::ofstream sFile(srcFile, std::ios::trunc | std::ios::binary);
        if (sFile.is_open()) {
            for (const auto& l : remainLines) {
                std::string utf8Line = ToUTF8(l) + "\r\n";
                sFile.write(utf8Line.c_str(), utf8Line.size());
            }
            sFile.flush();
            sFile.close();
        }

    }
    catch (...) {
        std::wstring wmsg = L"删除数据行失败";
        WriteError(wmsg, srcFile, 4);
    }
}

// 更新替换文件里匹配文本的行并立即写入
// 原始行内容的整行替换，适用出入参针对整行问题
// 如果是一行里某个元素的替换，可使用 UpdateLineElementByIndex 先生成新行，再调用本方法替换
void ReplaceLine(const std::wstring srcFile, const std::wstring  oldLine, const std::wstring newLine) {

    std::wstring inputOld = FormattedLine(oldLine);
    std::wstring inputNew = FormattedLine(newLine);

    if (inputOld.empty() || inputOld.find(g_delimiter) == std::wstring::npos || inputNew.empty() || inputNew.find(g_delimiter) == std::wstring::npos) {
        return;
    }

    if (srcFile.empty()) {
        std::wstring wmsg = L"ReplaceLine 失败：传入的源文件路径为空";
        WriteError(wmsg, srcFile, 4);
        return;
    }
    // 2. 检查文件是否存在
    if (!std::filesystem::exists(srcFile)) {
        std::wstring wmsg = L"ReplaceLine 失败：目标文件不存在";
        WriteError(wmsg, srcFile, 4);
        return;
    }


    try {

        {
            // 1. 独立锁控制：必须与所有读写该文件的其他方法使用同一个全局锁
            std::lock_guard<std::mutex> lock(g_queueMutex);

            std::wstring srcPath = srcFile;
            std::vector<std::wstring> fileContents = FileToWStringLinesXY(srcPath);

            std::vector<std::wstring> newContents;
            newContents.reserve(fileContents.size());

            bool isModified = false;

            for (const auto& line : fileContents) {
                std::wstring curFormattedLine = FormattedLine(line);

                if (curFormattedLine == inputOld) {
                    newContents.push_back(inputNew);
                    isModified = true;
                }
                else {
                    newContents.push_back(curFormattedLine); // 保留原始行
                }
            }



            // 4. 写入阶段：只有当内容发生变化时才写回磁盘
            if (isModified) {
                // 以二进制方式打开文件，覆盖原内容
                std::ofstream ofs(srcFile, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!ofs.is_open()) {
                    std::wstring wmsg = L"ReplaceLine 失败：无法打开文件写入更新";
                    WriteError(wmsg, srcFile, 4);
                    return;
                }

                for (size_t i = 0; i < newContents.size(); ++i) {
                    // 先将 std::wstring 转为 UTF-8 字符串
                    std::string utf8Line = ToUTF8(newContents[i]);
                    ofs << utf8Line;

                    // 仅在非最后一行或需要保持文件结尾换行时添加 \r\n
                    if (i < newContents.size()) {
                        ofs << "\r\n";
                    }
                }

                ofs.flush();
                ofs.close();
            }
        }

    }
    catch (const std::filesystem::filesystem_error& fe) {
        std::wstring wmsg = L"ReplaceLine 发生文件系统异常：" + ToWide(std::string(fe.what()));
        WriteError(wmsg, srcFile, 4);

    }
    catch (const std::exception& e) {
        std::wstring wmsg = L"ReplaceLine 发生未知异常：" + ToWide(std::string(e.what()));
        WriteError(wmsg, srcFile, 4);
    }
    catch (...) {
        std::wstring wmsg = L"ReplaceLine 发生无法识别的致命错误";

        WriteError(wmsg, srcFile, 4);
    }
}


// 用旧行整行在原文件里匹配，删除匹配行，并将替换行追加到新文件
void ReplaceLinesToNewFile(const std::wstring srcFile, const std::wstring oldLine, const std::wstring newLine, const std::wstring destFile)
{
    // 基础校验：oldLine 和 newLine 必须是有效格式化后的带双引号行
    std::wstring inputOld = FormattedLine(oldLine);
    std::wstring inputNew = FormattedLine(newLine);
    if (inputOld.empty() || inputOld.find(g_delimiter) == std::wstring::npos || inputNew.empty() || inputNew.find(g_delimiter) == std::wstring::npos) {
        return;
    }
    if (srcFile.empty() || destFile.empty()) {
        std::wstring wmsg = L"ReplaceLinesToNewFile 失败：源或目标文件路径为空";

        WriteError(wmsg, srcFile, 4);
        return;
    }

    // 检查源文件是否存在
    if (!std::filesystem::exists(srcFile)) {
        std::wstring wmsg = L"ReplaceLinesToNewFile 失败：源文件不存在";

        WriteError(wmsg, srcFile, 4);
        return;
    }

    if (!std::filesystem::exists(destFile)) {
        std::ofstream ofs(destFile);
        unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        ofs.write((char*)bom, 3);
        ofs.flush();
    }

    try
    {
        // 统一加锁：整个读 + 双写过程原子化（与 ReplaceLine 等其他操作共享同一锁）
        std::lock_guard<std::mutex> lock(g_queueMutex);

        std::wstring srcPath = srcFile;

        // 读取源文件所有行（宽字符串）
        std::vector<std::wstring> fileContents = FileToWStringLinesXY(srcPath);
        if (fileContents.empty() && !std::filesystem::exists(srcFile)) {
            // 文件为空或不存在，直接返回（已在上层判断存在，这里保险）
            return;
        }

        // 准备两个输出容器
        std::vector<std::wstring> remainingLines;  // 写回 srcFile（未匹配的行）
        std::vector<std::wstring> replacedLines;   // 追加到 destFile（匹配并替换的行）

        for (const auto& line : fileContents)
        {
            std::wstring curFormattedLine = FormattedLine(line);

            if (curFormattedLine == inputOld)
            {
                // 匹配成功 → 删除此行（不放 remainingLines），并追加一个替换行到 destFile
                replacedLines.push_back(inputNew);
            }
            else
            {
                // 未匹配 → 保留写回原文件
                remainingLines.push_back(curFormattedLine);  // 已格式化，可直接写
            }
        }

        // 只有发生变化才写文件（优化磁盘IO）
        bool hasReplaced = !replacedLines.empty();
        bool hasRemainingChange = (remainingLines.size() != fileContents.size());

        if (!hasReplaced && !hasRemainingChange)
        {
            // 没有匹配到任何行，无需写入
            return;
        }

        // ==================== 写入清理后的原文件 ====================
        // 目录使用全局 qPath（假设是 std::string）
        if (hasReplaced || hasRemainingChange)
        {
            WriteUTF8LinesToFile(srcFile, nullptr, remainingLines, 1);
        }

        // ==================== 追加替换行到目标文件 ====================
        if (hasReplaced)
        {
            WriteUTF8LinesToFile(destFile, nullptr, replacedLines, 2);
        }
    }
    catch (const std::filesystem::filesystem_error& fe)
    {
        std::wstring wmsg = L"ReplaceLinesToNewFile 发生文件系统异常：" + ToWide(std::string(fe.what()));

        WriteError(wmsg, srcFile, 4);
    }
    catch (const std::exception& e)
    {
        std::wstring wmsg = L"ReplaceLinesToNewFile 发生未知异常：" + ToWide(std::string(e.what()));

        WriteError(wmsg, srcFile, 4);
    }
    catch (...)
    {
        std::wstring wmsg = L"ReplaceLinesToNewFile 发生无法识别的致命错误";

        WriteError(wmsg, srcFile, 4);
    }
}











// 读取全局配置
void LoadGeneralConfig() {
    std::wstring configFilePath = cfgPath + L"/" + generalCFG;
    try {
        namespace fs = std::filesystem;
        // 1. 检查目录及文件是否存在
        if (!fs::exists(cfgPath)) {
            fs::create_directories(cfgPath);
        }
        if (!fs::exists(configFilePath)) {
            // 如果不存在则创建空文件
            std::ofstream outfile(configFilePath);
            outfile.flush();
            outfile.close();
        }

        // 2. 读取文件内容
        std::wstring content = GetFileContent(configFilePath);
        if (content.empty()) return;

        std::wstringstream wss(content);
        std::wstring rawLine;

        int cSuccess = 0;
        int cFailed = 0;

        while (std::getline(wss, rawLine)) {
            std::wstring line = FormattedLine(RemoveRemarkText(rawLine));
            if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;


            // ===== 格式化文本后必须以分割符开始 =====
            if (line[0] != g_delimiter) {
                ++cFailed;
                std::wstring wmsg = L"全局配置规则行格式非法" + line;
                // std::string msg = ToUTF8(wmsg);
                // WriteError(wmsg, line);
                loadGeneralConfigErrorMsg.insert(wmsg);
                continue;
            }

            // ===== 双引号数量必须成对 =====
            size_t quoteCount = std::count(line.begin(), line.end(), g_delimiter);
            if (quoteCount == 0 || quoteCount % 2 != 0) {
                ++cFailed;
                std::wstring wmsg = L"全局配置规则行格式非法：" + line;
                // std::string msg = ToUTF8(wmsg);
                // WriteError(wmsg, line);
                loadGeneralConfigErrorMsg.insert(wmsg);
                continue;
            }

            // 解析：Index 1 为变量名，Index 2 为配置值
            LineElements le = ReadLineElements(line, g_delimiter);
            std::wstring key = le.element1;
            std::wstring value = le.element2;

            if (key.empty()) continue;

            bool matched = true;
            // 3. 匹配映射逻辑
            // std::string 类型的变量映射
            if (key == L"cfgPath") cfgPath = value;
            else if (key == L"logPath") logPath = value;
            else if (key == L"qPath") qPath = value;
            else if (key == L"resPath") resPath = value;
            else if (key == L"filterCFG") filterCFG = value;
            else if (key == L"listenCFG") listenCFG = value;
            else if (key == L"generalCFG") generalCFG = value;
            else if (key == L"smbCFG") smbCredentialCFG = value;
            else if (key == L"fileLOGPrefix") fileLOGPrefix = value;



            else if (key == L"fileSuccessMiddle") fileSuccessMiddle = value;
            else if (key == L"fileRetryMiddle") fileRetryMiddle = value;
            else if (key == L"fileFailedMiddle") fileFailedMiddle = value;
            else if (key == L"fileDiscardedMiddle") fileDiscardedMiddle = value;

            else if (key == L"mergeLOG") mergeLOG = value;
            else if (key == L"counterRES") counterRES = value;
            else if (key == L"logPrefix") logPrefix = value;
            else if (key == L"errorPrefix") errorPrefix = value;

            else if (key == L"listenQ") listenQ = value;
            else if (key == L"tmpQ") tmpQ = value;
            else if (key == L"fileLOG") fileLOG = value;
            else if (key == L"mergePassedQ") mergeSuccessQ = value;
            else if (key == L"mergeFailedQ") mergeFailedQ = value;
            else if (key == L"mergeRawQ") mergeRawQ = value;
            else if (key == L"mergeBadQ") mergeBadQ = value;
            else if (key == L"mergeExQ") mergeExcludedQ = value;

            // 重试暂时失败的监听-目标目录对的重试间隔，0为不重试，默认值为2分钟
            else if (key == L"retryInterval") {
                retryIntervalW = value; // 和g_retryInterval成对，g_retryInterval用于实际逻辑
                std::chrono::minutes temp;
                if (value != L"0" && WStringToMinutes(value, temp)) {
                    g_retryInterval = temp;  // 成功设置非0值
                }
                else if (value == L"0") {
                    // 禁用重试
                    g_retryInterval = std::chrono::minutes(std::stoi(L"99999"));
                    std::wstring wmsg = L"目录注册的重试间隔配置为0，禁用重试";
                    loadGeneralConfigMsg.insert(wmsg);
                }
                else {
                    // 什么都不发生，保持默认值但retryIntervalW转义，用于加载监听目录时的日志打印
                    retryIntervalW = L"-1";
                    std::wstring wmsg = L"目录注册的重试间隔使用默认值：" + std::to_wstring(g_retryInterval.count()) + L"分钟";
                    loadGeneralConfigMsg.insert(wmsg);
                }

            }

            // 整型变量映射
            else if (key == L"disableExcludeQueueLog") disableExcludeQueueLog = std::stoi(value);
            else if (key == L"disableCorrectQueueLog") disableCorrectQueueLog = std::stoi(value);
            else if (key == L"disableModeCurrentAndFileLog") disableModeCurrentAndFileLog = std::stoi(value);
            else if (key == L"disableFileQueueLog") disableFileQueueLog = std::stoi(value);

            else if (key == L"logLevel") g_logLevel = std::stoi(value);
            else if (key == L"DebounceWaitMilli") DebounceWaitMilli = std::stoi(value);
            else if (key == L"DebounceRenameWaitMilli") DebounceRenameWaitMilli = std::stoi(value);
            else if (key == L"initDefaultId") { long long val = std::stoll(value); initDefaultId = val; }
            else if (key == L"mergeDelayMilli") mergeDelayMilli = std::stoi(value);
            else if (key == L"mergeRetryMilli") mergeRetryMilli = std::stoi(value);
            else if (key == L"mergeRetryTimes") mergeRetryTimes = std::stoi(value);
            else if (key == L"queueFirstWindow") MergeQueueCtx.firstRoundTimeWindow = std::stoi(value);
            else if (key == L"queueRoundWindow") MergeQueueCtx.roundTimeWindow = std::stoi(value);

            else if (key == L"fileCopyOption") fileCopyOption = std::stoi(value);
            else if (key == L"fileCopyRemoveLock") fileCopyRemoveLock = std::stoi(value);
            else if (key == L"fullSyncOnLoad") fullSyncOnLoad = std::stoi(value);
            else if (key == L"fileCopyOptionOnLoad") fileCopyOptionOnLoad = std::stoi(value);
            else if (key == L"serviceDelayStartInSeconds") serviceDelayStartInSeconds = std::stoi(value);


            // std::wstring 类型的变量映射

            else if (key == L"supportedListenType") supportedListenType = value;
            else if (key == L"typeFILE") typeFILE = value;
            else if (key == L"typeFOLDER") typeFOLDER = value;
            else if (key == L"actionADD") actionADD = value;
            else if (key == L"actionMOD") actionMOD = value;
            else if (key == L"actionDEL") actionDEL = value;
            else if (key == L"actionRENOLD") actionRENOLD = value;
            else if (key == L"actionRENNEW") actionRENNEW = value;
            else if (key == L"actionUNKNOWN") actionUNKNOWN = value;
            else if (key == L"actionADDR") actionADDR = value;
            else if (key == L"actionDELR") actionDELR = value;
            else if (key == L"actionRENOLDR") actionRENOLDR = value;
            else if (key == L"actionRENNEWR") actionRENNEWR = value;

            else if (key == L"statusINIT") aStatusINIT = value;
            else if (key == L"statusMSUCCESS") aStatusMSUCCESS = value;
            else if (key == L"statusMBAD") aStatusMBAD = value;
            else if (key == L"statusMRETRY") aStatusMRETRY = value;
            else if (key == L"statusMFAILED") aStatusMFAILED = value;
            else if (key == L"statusMREMOVED") aStatusMREMOVED = value;
            else if (key == L"statusMEXCLUDED") aStatusMEXCLUDED = value;
            else if (key == L"statusMCORRECTED") aStatusMCORRECTED = value;
            else if (key == L"statusMUNKNOWN") aStatusMUNKNOWN = value;
            else if (key == L"statusMMERGED") aStatusMMERGED = value;

            else if (key == L"statusPSUCCESS") aStatusPSUCCESS = value;
            else if (key == L"statusPRETRY") aStatusPRETRY = value;
            else if (key == L"statusPFAILED") aStatusPFAILED = value;

            else {
                matched = false;
                std::wstring wmsg = L"该配置项不支持，跳过：" + key + L"，值：" + value;
                ++cFailed;
                loadGeneralConfigErrorMsg.insert(wmsg);
            }

            if (matched) {
                ++cSuccess;
            }
        }
        std::wstring tmsg = L"加载全局/一般配置，" + configFilePath + L"，成功加载" + std::to_wstring(cSuccess) + L"个配置项，失败" + std::to_wstring(cFailed) + L"个";
        loadGeneralConfigMsg.insert(tmsg);
    }
    catch (...) {
        std::wstring wmsg = L"加载全局配置发生异常";
        loadGeneralConfigErrorMsg.insert(wmsg);
    }
}













// --- 核心监控逻辑 ---

// 注册文件夹监听
/* ================= RegisterDir（修正版） ================= */
void RegisterDir(const std::wstring& path) {
    HANDLE hDir = CreateFileW(
        path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        std::wstring wmsg = L"无法打开监听目录，错误码：" + std::to_wstring(GetLastError());

        WriteError(wmsg, path, 4);
        return;
    }
    else {

    }

    auto* ctx = new DirContext{};
    ctx->hDir = hDir;
    ctx->wPath = path;
    ctx->io.ctx = ctx;

    CreateIoCompletionPort(hDir, g_hCompPort, (ULONG_PTR)ctx, 0);

    {
        std::lock_guard<std::mutex> lk(g_dirMutex);
        g_allDirs.push_back(ctx);
    }

    ReadDirectoryChangesW(
        hDir,
        ctx->buffer,
        BUFFER_SIZE,
        TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
        nullptr,
        &ctx->io.ov,
        nullptr
    );
}

// 加载SMB用户权限配置
void LoadCredentialConfig() {
    std::wstring configFilePath = cfgPath + L"/" + smbCredentialCFG;

    try {
        namespace fs = std::filesystem;

        // 1. 检查目录及文件
        fs::path cfgDir(cfgPath);
        if (!fs::exists(cfgDir)) {
            fs::create_directories(cfgDir);
        }

        if (!fs::exists(configFilePath)) {
            // 创建空文件
            std::ofstream ofs(configFilePath);
            unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            ofs.write((char*)bom, 3);
            ofs.flush();
            ofs.close();
        }

        // 2. 读取整个文件为行
        std::vector<std::wstring> lines = FileToWStringLinesXY(configFilePath);
        if (lines.empty()) return;

        int successCount = 0;
        int failCount = 0;
        int duplicateCount = 0;
        int inCredentialSet = 0;

        for (auto& rawLine : lines) {
            std::wstring line = FormattedLine(RemoveRemarkText(rawLine));

            // 跳过空行或注释行
            if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;


            // ===== 格式化文本后必须以分割符开始 =====
            if (line[0] != g_delimiter) {
                ++failCount;
                std::wstring wmsg = L"凭证配置规则行格式非法";
                WriteError(wmsg, line, 4);
                continue;
            }

            // ===== 双引号数量必须成对 =====
            size_t quoteCount = std::count(line.begin(), line.end(), g_delimiter);
            if (quoteCount == 0 || quoteCount % 2 != 0) {
                ++failCount;
                std::wstring wmsg = L"凭证配置规则行格式非法";
                WriteError(wmsg, line, 4);
                continue;
            }

            try {
                // 解析三列数据：UNC + 用户名 + 密码
                LineElements le = ReadLineElements(line, g_delimiter);
                std::wstring uncServer = le.element1;
                std::wstring username = le.element2;
                std::wstring password = le.element3;

                // 基本校验
                if (uncServer.empty() || username.empty()) {
                    std::wstring wmsg = L"用户凭据配置行校验失败，跳过行 -> " + uncServer;
                    WriteError(wmsg, configFilePath, 3);
                    ++failCount;
                    continue;
                }

                inCredentialSet = countCredentialByUncServer(uncServer);
                if (inCredentialSet > 0) {
                    std::wstring wmsg = L"用户凭据配置行重复（该SMB的首行配置才有效），忽略行 -> " + uncServer;
                    WriteError(wmsg, configFilePath, 3);
                    ++duplicateCount;
                    continue;
                }
                // 有效行加入全局集合
                userCredential cred{ uncServer, username, password };
                g_configuredCredential.insert(cred);
                ++successCount;
            }
            catch (...) {
                std::wstring wmsg = L"加载凭据配置文件解析异常，跳过当前行 -> " + line;
                WriteError(wmsg, configFilePath, 4);
                ++failCount;
                continue;
            }
        }

        // 日志打印加载结果
        std::wstring tmsg = L"加载 SMB 凭据配置文件：" + configFilePath + L"，成功加载 " + std::to_wstring(successCount) +
            L" 条，失败 " + std::to_wstring(failCount) + L" 条，忽略重复" + std::to_wstring(duplicateCount) + L"条 ";
        WriteLog(tmsg, 5);
    }
    catch (...) {
        std::wstring wmsg = L"加载凭据配置文件发生异常";
        WriteError(wmsg, configFilePath, 4);
    }
}


// 建立所有配置好的 UNC SMB 会话（一次性连接，供后续所有操作复用）
void StartUNCSession()
{
    if (g_configuredCredential.empty()) {
        return;
    }
    try {
        int successCount = 0;
        int failedCount = 0;
        int skipCount = 0;
        for (const auto& cred : g_configuredCredential) {
            if (cred.uncServer.empty() || cred.username.empty()) {
                // 凭据不完整，跳过并记录警告
                ++failedCount;
                continue;
            }
            // 获取标准化根路径（如 \\server 或 \\server\share）
            std::wstring rootPath = getFormattedRootPath(cred.uncServer);
            if (rootPath.empty()) {
                ++failedCount;
                continue;
            }

            // ============ 检查是否已存在有效连接 ============
            bool alreadyConnected = false;
            HANDLE hEnum = nullptr;
            DWORD dwResult = WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, &hEnum);
            if (dwResult == NO_ERROR && hEnum != nullptr) {
                DWORD dwCount = -1;
                DWORD dwBufferSize = 16384;
                std::vector<BYTE> buffer(dwBufferSize);
                LPNETRESOURCEW lpnr = reinterpret_cast<LPNETRESOURCEW>(buffer.data());

                dwResult = WNetEnumResourceW(hEnum, &dwCount, lpnr, &dwBufferSize);
                if (dwResult == NO_ERROR) {
                    for (DWORD i = 0; i < dwCount; ++i) {
                        std::wstring remoteName = lpnr[i].lpRemoteName ? lpnr[i].lpRemoteName : g_emptyWString;
                        if (_wcsicmp(remoteName.c_str(), rootPath.c_str()) == 0) {
                            alreadyConnected = true;
                            break;
                        }
                    }
                }
                WNetCloseEnum(hEnum);
            }

            if (alreadyConnected) {
                WriteFileSyncLog(L"StartUNCSession：UNC 会话已存在，跳过重复连接：" + rootPath, rootPath, g_emptyWString, 5, 3, 1);
                ++skipCount;
                continue;
            }

            NETRESOURCEW nr{};
            nr.dwType = RESOURCETYPE_DISK;
            nr.lpLocalName = nullptr;                    // 不映射驱动器号
            nr.lpRemoteName = const_cast<LPWSTR>(rootPath.c_str());
            nr.lpProvider = nullptr;

            DWORD flags = 0;
            flags |= CONNECT_UPDATE_PROFILE;

            // 建立连接（CONNECT_TEMPORARY 表示临时连接，程序结束时自动断开；也可去掉该标志做持久连接）
            // 如果还想映射驱动器号（如 Z:），可以加：
             // flags |= CONNECT_LOCALDRIVE | CONNECT_REDIRECT;
            DWORD result = WNetAddConnection2W(&nr,
                cred.password.empty() ? nullptr : cred.password.c_str(),
                cred.username.empty() ? nullptr : cred.username.c_str(),
                flags);
            // CONNECT_TEMPORARY

            if (result == NO_ERROR || result == ERROR_ALREADY_ASSIGNED) {
                // NO_ERROR：成功；ERROR_ALREADY_ASSIGNED：已存在连接（视为成功）
                ++successCount;
            }
            else {
                // 连接失败，记录系统错误码
                std::wstring wmsg = L"StartUNCSession：建立 UNC 会话失败，错误码：" + std::to_wstring(result) + L"，路径：" + rootPath;
                WriteError(wmsg, rootPath, 4);
                ++failedCount;
            }
        }
    }
    catch (...) {
        WriteError(L"StartUNCSession 过程中发生异常", g_emptyWString, 4);
    }
}

// 断开所有已建立的 UNC SMB 会话
void StopUNCSession()
{
    if (g_configuredCredential.empty()) {
        return;
    }

    try {
        int successCount = 0;
        int failedCount = 0;
        int skipCount = 0;

        for (const auto& cred : g_configuredCredential) {
            if (cred.uncServer.empty()) continue;

            std::wstring rootPath = getFormattedRootPath(cred.uncServer);
            if (rootPath.empty()) {
                ++failedCount;
                continue;
            }


            bool isConnected = false;
            HANDLE hEnum = nullptr;
            DWORD dwResult = WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, &hEnum);
            if (dwResult == NO_ERROR && hEnum != nullptr) {
                DWORD dwCount = -1;
                DWORD dwBufferSize = 16384;
                std::vector<BYTE> buffer(dwBufferSize);
                LPNETRESOURCEW lpnr = reinterpret_cast<LPNETRESOURCEW>(buffer.data());

                dwResult = WNetEnumResourceW(hEnum, &dwCount, lpnr, &dwBufferSize);
                if (dwResult == NO_ERROR) {
                    for (DWORD i = 0; i < dwCount; ++i) {
                        std::wstring remoteName = lpnr[i].lpRemoteName ? lpnr[i].lpRemoteName : g_emptyWString;
                        if (_wcsicmp(remoteName.c_str(), rootPath.c_str()) == 0) {
                            isConnected = true;
                            break;
                        }
                    }
                }
                WNetCloseEnum(hEnum);
            }

            if (!isConnected) {
                WriteFileSyncLog(L"StopUNCSession：UNC 会话已断开或不存在，跳过：" + rootPath, rootPath, g_emptyWString, 5, 3, 1);
                ++skipCount;
                continue;
            }

            // 强制断开连接（TRUE 表示强制，即使有打开的文件）
            DWORD result = WNetCancelConnection2W(rootPath.c_str(), 0, TRUE);

            if (result == NO_ERROR) {
                ++successCount;
            }
            else if (result == ERROR_NOT_CONNECTED) {
                // 已经没有连接，视为正常
                ++successCount;
            }
            else {
                std::wstring wmsg = L"StopUNCSession：断开 UNC 会话失败，错误码：" + std::to_wstring(result) +
                    L"，路径：" + rootPath;
                WriteError(wmsg, rootPath, 4);
                ++failedCount;
            }
        }

        WriteLog(L"StopUNCSession 完成：成功/已无连接 " + std::to_wstring(successCount) + L" 个，失败 " + std::to_wstring(failedCount) + L" 个", 1);
    }
    catch (...) {
        WriteError(L"StopUNCSession 过程中发生异常", g_emptyWString, 4);
    }
}





/* ================= 加载监听目录清单 ================= */
void LoadListenConfig() {
    // 直接使用全局变量，将其转换为规范的绝对路径对象
    fs::path exeDirPath = fs::absolute(g_exeRoot);
    std::wstring configPath = cfgPath + L"/" + listenCFG;
    int cSuccess = 0; int cFailed = 0; int cSkipped = 0; int cIrrelevant = 0; int cRetry = 0; int cExAdded = 0;
    std::set<std::pair<std::wstring, int>> loadedListenFolders;
    
    std::vector<std::wstring> lines = FileToWStringLinesXY(configPath);

    for (const auto& sline : lines) {

        std::wstring line = FormattedLine(RemoveRemarkText(sline));



        // 跳过空行、注释行
        if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) {
            ++cIrrelevant;
            continue;
        }

        // ===== 格式化文本后必须以分割符开始 =====
        if (line[0] != g_delimiter) {
            ++cIrrelevant;
            continue;
        }

        // ===== 2. 双引号数量必须成对 =====
        size_t quoteCount = std::count(line.begin(), line.end(), g_delimiter);
        if (quoteCount == 0 || quoteCount % 2 != 0) {
            ++cIrrelevant;
            continue;
        }


        auto row = ParseListenConfigLine(line);
        auto listenType = row.listenType;
        auto listenPath = row.sourceFolder;
        auto destSyncPath = row.destFolder;
        auto syncMode = row.syncMode;
        auto excludedPaths = row.exclusion;  // 排除清单，是一个目录集合

        listenPath = FormatPath(listenPath);
        destSyncPath = FormatPath(destSyncPath);

        auto originalListenPath = listenPath;  // 单文件完整路径，用于全局变量g_syncDirPair操作
        std::wstring singleFileName;
        // 如果指定为监听文件，这里还是要转为父目录，因为向windows注册监听需要是目录
        if (syncMode == syncModeSingleFile) {
            listenPath = GetParentPathNoBackSlash(listenPath);  // 变量转为文件的父目录用于后面代码
            singleFileName = GetFileOrFolderName(originalListenPath); // 主要后面打印用
        }

        std::wstring modeMsg = (syncMode == syncModeFull) ? L"全量" : (syncMode == syncModeCurrent) ? L"仅目录下文件" : L"单文件：" + singleFileName;

        InnerSyncPair currentPairForInner = { originalListenPath , destSyncPath, syncMode };
        SyncDir matchedSyncDir;
        std::set<InnerSyncPair> InnerPairs;


        // 跳过配置类型不支持的行
        // 配置不管怎么指定，如果是文件的话用了全局typeFILE，所以自定义supportedListenType没什么意义，还是只认固定文本FILE，剩余的算作目录
        std::vector<std::wstring> supportedListenTypes = SplitByDelimitedField(supportedListenType, L',');
        bool inTypes = false;
        for (const auto& slt : supportedListenTypes) {
            if (listenType == slt) {
                inTypes = true;
                break;
            }
        }
        if (!inTypes) {
            std::wstring wmsg = originalListenPath + L"，配置行类型为：" + listenType + L" ，不在允许范围内：" + supportedListenType;
            WriteError(wmsg, originalListenPath, 3);
            ++cFailed;
            continue;
        }

        // 1. 规范化配置路径
        fs::path linePath(listenPath);
        fs::path absoluteLinePath;
        try {
            absoluteLinePath = fs::absolute(linePath);
        }
        catch (...) {
            std::wstring wmsg = L"监听路径格式非法，无法解析";
            WriteError(wmsg, originalListenPath, 4);
            ++cFailed;
            continue;
        }

        if (!DirectoryExistsNoCredential(absoluteLinePath)) {
            std::wstring wmsg = L"监听目录不存在或无法访问";
            WriteError(wmsg, originalListenPath, 4);
            ++cFailed;
            continue;
        }

        // 每条配置记录检查是否已经在loadedListenFolders中已经存在已注册监听目录
        bool existListenFolder = false;
        auto it = loadedListenFolders.lower_bound({ listenPath, INT_MIN });  // 查找第一个 >= (wstring_to_check, min int)
        if (it != loadedListenFolders.end() && it->first == listenPath) {
            existListenFolder = true;
        }

        if (syncMode == syncModeSingleFile) {

            bool existInPairs = false;
            bool duplicatedPair = false; //不局限于文件类型，内层记录包含所有同步类型，只要重复就不允许
            bool accessibleDest = DirectoryExistsWithCredential(destSyncPath, 1);
            for (auto& pair : g_syncDirPair) {
                if (pair.listenPath == listenPath) {  // 父目录在pair中已有
                    existInPairs = true;
                    matchedSyncDir = pair;
                    InnerPairs = matchedSyncDir.innerPair;

                    if (pair.destSyncPath == destSyncPath) duplicatedPair = true;

                    for (const auto& inner : pair.innerPair) {
                        // 文件路径已存在于监听目录对的innerPair清单
                        if (inner.innerSyncMode == syncModeSingleFile && inner.innerSrcPath == originalListenPath && inner.innerDestPath == destSyncPath) {
                            duplicatedPair = true;
                            break;
                        }
                        // 文件的父目录即监听路径已经存在其它非FILE注册成功且目标相同的记录
                        else if (inner.innerSyncMode != syncModeSingleFile && inner.innerSrcPath == listenPath && inner.innerDestPath == destSyncPath) {
                            duplicatedPair = true;
                            break;
                        }
                    }
                    // 如果父目录已经注册成功在g_syncDirPair里而文件还没添加过，直接加
                    if (!duplicatedPair && accessibleDest) {
                        // 直接操作g_syncDirPair已注册目录的innerPair，加一对该文件的目录对
                        pair.innerPair.insert(currentPairForInner);
                    }
                    break;
                }
            } // 全局pair循环结束


            if (existInPairs) {
                // 无论普通还是file类型，只要在内外层重复过了就失败
                if (duplicatedPair) {
                    ++cSkipped;
                    std::wstring wmsg = L"指定文件：" + singleFileName + L" ，及目标：" + destSyncPath + L" ，和现有成功配置重复，忽略冲突配置";
                    WriteLog(wmsg, 3);
                    continue;
                }
                // 校验目录是否存在
                if (!accessibleDest) {
                    ++cFailed;
                    std::wstring wmsg = L"目标目录当前无法访问或无权限（SMB），本行加载失败，也无重试。源：" + originalListenPath + L"，目标：" + destSyncPath;
                    WriteError(wmsg, g_emptyWString, 4);
                }
                else {
                    // 满足已存在父目录、文件还没添加到innerPair情况，在上面的循环中已经操作过pair.innerPair.insert，这里直接算成功并跳过本条配置
                    // 且不发起后面的一串针对目录注册的判定

                    // 一个监听目录的符合校验的配置次数加1
                    auto it = loadedListenFolders.lower_bound({ listenPath, INT_MIN });
                    int new_value;
                    if (it != loadedListenFolders.end() && it->first == listenPath) {
                        new_value = it->second + 1;
                        loadedListenFolders.erase(it);
                        loadedListenFolders.insert({ listenPath, new_value });
                    }
                    ++cSuccess;
                    std::wstring wmsg = L"单文件：" + singleFileName + L"，成功添加到目录：" + listenPath + L"        ========> [" + std::to_wstring(new_value) + L"#]        " + destSyncPath;
                    WriteLog(wmsg, 5);

                    // 成功后添加本行配置例外路径
                    if (!excludedPaths.empty()) {
                        for (const auto& exPath : excludedPaths) {
                            std::wstring formattedExPath = FormatPath(exPath);
                            AddSingleExcludeFolderToMem(formattedExPath);
                            std::wstring wmsg = L"添加例外路径成功：" + formattedExPath;
                            WriteLog(wmsg, 5);
                            ++cExAdded;
                        }
                    }
                }
                continue;

            }
            else {
                // 两个都不存在，InnerPairs也加一下文件路径，走后面的父目录正常注册流程，也需要经过一系列的冲突判断
                // 首次添加时，pair的目标路径和pair下的innerPair的目标路径一样，将来file类型的配置条目，实际拿来复制用的目标需要改为innerPair内目标
                // 和下面文件级主要的区别在于，文件级的首次注册目录，内层就要添加该文件pair，而目录首次注册时不需要。外层主要是给监听向windows注册用的，只有目录没有文件
                InnerPairs.insert(currentPairForInner);
            }
        } // 当前行是FILE情况下的预处理：路径已转为父目录、在g_syncDirPair中的父目录注册情况、文件路径是否存在的情况已判断


        

        // 普通模式当前记录如果存在记录，先检查类型是否syncModeSingleFile，是的话不发起注册，直接把类型修改为普通
        if (existListenFolder && syncMode != syncModeSingleFile) {
            bool existListen = false; // 表示目录在windows已注册
            std::wstring pairSyncMode;
            bool duplicatedDest = false; // 表示相同dest的记录已存在，不区分文件和非文件两类，外层、内存都用这个，任意一个存在就成立
            bool accessibleDest = DirectoryExistsWithCredential(destSyncPath, 1);
            for (auto& pair : g_syncDirPair) {
                if (listenPath == pair.listenPath) {

                    pairSyncMode = pair.syncMode;
                    if (destSyncPath == pair.destSyncPath) duplicatedDest = true;

                    if (pair.innerPair.size() > 0) {
                        for (auto& inner : pair.innerPair) {
                            if (inner.innerDestPath == destSyncPath) {
                                // 只判断目标，也包括FILE类型也不能一样，否则在列表里的文件就满足两条目标相同的记录
                                duplicatedDest = true;
                                break;
                            }
                        }
                    }

                    // 只有目录可访问时才执行
                    // 只有当前监听类型是FILE时才直接更新，因为FILE记录在inner已经存在。否则外层不更新，往内层里加
                    if (!duplicatedDest && pair.syncMode == syncModeSingleFile && accessibleDest) {
                        pair.syncMode = syncMode;
                        pair.destSyncPath = destSyncPath;
                    }
                    // 否则加入内层列表
                    else if (!duplicatedDest && pair.syncMode != syncModeSingleFile && accessibleDest) {
                        pair.innerPair.insert(currentPairForInner);
                    }
                    existListen = true; // 检测监听目录是否已存在
                    break;
                }
            }

            // 已有监听、目标存在任何重复（内外层，含文件和非文件），失败
            if (existListen && duplicatedDest) {
                ++cSkipped;
                std::wstring wmsg = L"配置和已有配置目标相同，忽略。源：" + listenPath + L"，目标：" + destSyncPath;
                WriteError(wmsg, g_emptyWString, 4);
                continue;
            }
            // 已有监听，目标不存在重复，pair记录是FILE类型，则成功，上面循环代码时已把外层更新为本次配置的同步类型、dest
            else if (existListen && !duplicatedDest && pairSyncMode == syncModeSingleFile) {
                // 校验目录是否存在
                if (!accessibleDest) {
                    ++cFailed;
                    std::wstring wmsg = L"目标目录当前无法访问或无权限（SMB），加载失败。源：" + originalListenPath + L"，目标：" + destSyncPath;
                    WriteError(wmsg, g_emptyWString, 4);
                }
                else {
                    // 一个监听目录的符合校验的配置次数加1
                    auto it = loadedListenFolders.lower_bound({ listenPath, INT_MIN });
                    int new_value;
                    if (it != loadedListenFolders.end() && it->first == listenPath) {
                        new_value = it->second + 1;
                        loadedListenFolders.erase(it);
                        loadedListenFolders.insert({ listenPath, new_value });
                    }
                    ++cSuccess;
                    std::wstring wmsg = L"添加多目标成功，注册目录转为" + modeMsg + L"模式：" + listenPath + L"        ========> [" + std::to_wstring(new_value) + L"#]        " + destSyncPath;
                    WriteLog(wmsg, 5);
                    // 成功后添加本行配置例外路径
                    if (!excludedPaths.empty()) {
                        for (const auto& exPath : excludedPaths) {
                            std::wstring formattedExPath = FormatPath(exPath);
                            AddSingleExcludeFolderToMem(formattedExPath);
                            std::wstring wmsg = L"添加例外路径成功：" + formattedExPath;
                            WriteLog(wmsg, 5);
                            ++cExAdded;
                        }
                    }
                }
                continue;
            }
            // 已有监听，目标不存在重复，pair记录是非FILE类型，也成功，上面循环代码时已把本次配置加入内层列表
            else if (existListen && !duplicatedDest && pairSyncMode != syncModeSingleFile) {
                // 校验目录是否存在
                if (!accessibleDest) {
                    ++cFailed;
                    std::wstring wmsg = L"目标目录当前无法访问或无权限（SMB），加载失败。源：" + originalListenPath + L"，目标：" + destSyncPath;
                    WriteError(wmsg, g_emptyWString, 4);
                }
                else {
                    // 一个监听目录的符合校验的配置次数加1
                    auto it = loadedListenFolders.lower_bound({ listenPath, INT_MIN });
                    int new_value;
                    if (it != loadedListenFolders.end() && it->first == listenPath) {
                        new_value = it->second + 1;
                        loadedListenFolders.erase(it);
                        loadedListenFolders.insert({ listenPath, new_value });
                    }
                    ++cSuccess;
                    std::wstring wmsg = L"添加多目标成功（" + modeMsg + L"）：" + listenPath + L"        ========> [" + std::to_wstring(new_value) + L"#]        " + destSyncPath;
                    WriteLog(wmsg, 5);
                    // 成功后添加本行配置例外路径
                    if (!excludedPaths.empty()) {
                        for (const auto& exPath : excludedPaths) {
                            std::wstring formattedExPath = FormatPath(exPath);
                            AddSingleExcludeFolderToMem(formattedExPath);
                            std::wstring wmsg = L"添加例外路径成功：" + formattedExPath;
                            WriteLog(wmsg, 5);
                            ++cExAdded;
                        }
                    }
                }
                continue;
            }
            else {
                // 逻辑上应该else情况只剩当前记录在已注册目录中不存在，走下面的新注册目录流程
                // currentPairForInner = {};
            }
        }



        // 2. 核心逻辑判断：
        // 使用 std::search 或简单的路径包含关系检查
        // 如果 absoluteConfigPath 是 exeDirPath 的父级或相同
        // 调整为只限定log和queue目录及它们的子目录
        std::wstring sExeDir = exeDirPath.wstring();
        std::wstring sCfgDir = absoluteLinePath.wstring();
        std::wstring sExeDir1 = sExeDir + L"\\" + logPath;
        std::wstring sExeDir2 = sExeDir + L"\\" + qPath;

        bool isNestedOrSelfConflict = sExeDir1.find(sCfgDir) == 0 || sCfgDir.find(sExeDir1) == 0 || sExeDir2.find(sCfgDir) == 0 || sCfgDir.find(sExeDir2) == 0;

        // 检查配置路径是否包含了程序所在路径 (自监听检查)
        // if (sExeDir.find(sCfgDir) == 0 || sCfgDir.find(sExeDir) == 0) {
        if (isNestedOrSelfConflict) {
            std::wstring wmsg = L"跳过非法配置：" + originalListenPath + L"，程序运行会更新文件的目录监听会产生死锁";
            WriteLog(wmsg, 5);
            WriteError(wmsg, originalListenPath, 3);
            ++cFailed;
            continue;
        }


        if (!listenPath.empty()) {
            // 这里不打印，放到 RegisterDir 内打印更准确
            // 记录已加载的监听目录，避免重复加载，因为windows会重发发多次监听通知消息扰乱整个处理队列，要严格剔重
            if (!existListenFolder) {

                if (DirectoryExistsWithCredential(destSyncPath, 1)) {

                    bool dupDest = ExistsDestInCollection(g_syncDirPair, listenPath, destSyncPath);
                    // 非文件模式必须要目标无冲突；文件模式则不限制
                    if ((!dupDest && syncMode != syncModeSingleFile) || syncMode == syncModeSingleFile) {

                        // ==================== 父/子目录冲突检测，针对监听目录 ====================
                        bool conflict = false;
                        for (const auto& existing : g_syncDirPair) {
                            // 当前配置是 existing 的子目录
                            if (ContainSubFolderOrEqual(listenPath, existing.listenPath) || ContainSubFolderOrEqual(existing.listenPath, listenPath)) {
                                conflict = true;
                                break;
                            }
                        }

                        if (conflict) {
                            std::wstring wmsg = L"已有其它父/子目录成功注册，本条不成功，需修改正确配置";
                            WriteError(wmsg, originalListenPath, 3);
                            wmsg = L"配置冲突：与已注册目录存在父子包含关系，跳过注册。源目录：" + listenPath + L"，目标：" + destSyncPath;
                            WriteLog(wmsg, 3);
                            ++cFailed;
                            continue;  // 跳过本条配置，不注册、不重试
                        }

                        loadedListenFolders.insert({ listenPath , 1});
                        ++cSuccess;
                        // 记录到全局变量，用于后续删除目录时正确判断对象类型为目录，因为初始判断机制不准
                        AddFolderTreeToMem(listenPath);
                        // 记录到有效的监听 -> 同步目标的目录对
                        // 如果是FILE类型，该pair首次添加FILE路径到innerPair集合，非FILE InnerPairs为空
                        // syncMode是配置行里的类型，在手册注册成功到全局pair上时，类型相同，后续便不会再更改了，full、current、file这三种类型在一个路径（文件时的父）上互斥
                        // 20260108支持非FILE时多目标，这里的代码都是一个监听目录首次注册时，因此入参都是本条配置数据本身
                        // InnerPairs则按本配置是否为FILE，是的话也有一条相同配置的数据，不是的话则为空，因为初始的监听目录的目标在外层，内层只添加后面新配置的不同目标
                        AddToCollection(SYNC_DIR_PAIR, listenPath, destSyncPath, syncMode, InnerPairs);
                        RegisterDir(listenPath);


                        std::wstring wmsg = L"成功注册监听目录（" + modeMsg + L"）：" + listenPath + L"        ========> [1#]        " + destSyncPath;
                        WriteLog(wmsg, 5);

                        // 排除目录如果存在则添加到内存，但不校验目录本身的有效性，只作为wstring存在，后续交给文件队列处理时判定
                        // 因为在服务启动的声明周期里，原本不存在的排除目录可能会被添加出来，而这里的配置是一开始就加载了是静态的
                        for (const auto& exPath : excludedPaths) {
                            std::wstring formattedExPath = FormatPath(exPath);
                            AddSingleExcludeFolderToMem(formattedExPath);
                            std::wstring wmsg = L"添加例外路径成功：" + formattedExPath;
                            WriteLog(wmsg, 5);
                            ++cExAdded;
                        }
                        continue; // 为成功了的添加continue，识别到成功配置退出该行应该没问题
                    } // 目标目录无重复注册
                    else {
                        std::wstring wmsg = L"源目录可注册监听，但不同的源目录指向相同目标目录，配置丢弃且不会发起重试。源目录：" + listenPath + L"，目标：" + destSyncPath;
                        WriteError(wmsg, destSyncPath, 3);
                        ++cFailed;
                    }


                } // 目标可访问的if
                else {
                    std::wstring wmsg = L"目标非有效目录或当前无法访问、无权限（含SMB），对SMB确保有正确凭据配置。本行加载失败，源：" +
                        listenPath + L"，目标：" + destSyncPath;
                    WriteError(wmsg, destSyncPath, 4);
                    ++cFailed;
                    int ritv = std::stoi(retryIntervalW);

                    if (ritv > 0) {
                        // 进入目录对重试机制
                        std::wstring wmsg = L"源目录：" + listenPath + L" 有效，目标目录：" + destSyncPath +
                            L" 暂时无法访问进入重试。如确定目标目录不存在可以删除本行配置、或者配置文件里禁用重试功能";
                        WriteLog(wmsg, 5);
                        // 记录到重试的监听 -> 同步目标的目录对
                        // 重试也把InnerPairs带过去，否则会丢失配置。如果是FILE类型，该pair首次添加FILE路径到innerPair集合，非FILE InnerPairs为空
                        AddToCollection(SYNC_DIR_PAIR_RETRY, listenPath, destSyncPath, syncMode, InnerPairs);
                        // 已经在失败中统计过，统计时也会加在失败部分的括号中
                        ++cRetry;
                        // 启动异步重试线程，走重试机制，重试间隔使用全局配置重试间隔
                        // 在重试方法中，如果到最后重试中的目录对成功注册了监听，则会从重试集合中移除该目录对，加入正式监听集合，否则继续保留在重试集合中
                        // RetrySyncDirPair();
                        XYEventScheduler(RetryRegisterDirCtx, 1, EventProcessType::RetrySyncDirPair);
                    }
                    else if (ritv == 0) {
                        std::wstring wmsg = L"源目录：" + listenPath + L"，有效，目标目录：" + destSyncPath + L"，暂时无法访问。由于重试功能配置为禁用，不再发起重试";
                        WriteLog(wmsg, 3);
                    }
                    else if (ritv == -1) {
                        std::wstring wmsg = L"源目录：" + listenPath + L"，有效，目标目录：" + destSyncPath + L"，暂时无法访问。按系统默认间隔：" + std::to_wstring(g_retryInterval.count()) + L"分钟发起重试";
                        WriteLog(wmsg, 5);
                        ++cRetry;
                        // 走系统默认规则也需要重试，只是间隔不同
                        // RetrySyncDirPair();
                        XYEventScheduler(RetryRegisterDirCtx, 1, EventProcessType::RetrySyncDirPair);
                    }

                    // 添加例外路径
                    for (const auto& exPath : excludedPaths) {
                        std::wstring formattedExPath = FormatPath(exPath);
                        AddSingleExcludeFolderToMem(formattedExPath);
                        std::wstring wmsg = L"添加例外路径成功：" + formattedExPath;
                        WriteLog(wmsg, 5);
                        ++cExAdded;
                    }

                    continue;
                } // 目标不可访问时
            } // 监听目录还未添加时
            else {
                ++cSkipped;
                std::wstring wmsg = L"监听目录：" + listenPath + L" 已经注册，忽略重复配置";
                WriteLog(wmsg, 5);
                WriteError(wmsg, listenPath, 3);

            } // 监听目录已经添加过

        } // 无实际意义，监听目录不为空的if
    } // 循环每个配置行结束

    int cTotal = cSkipped + cFailed + cSuccess + cIrrelevant;
    std::wstring wmsg = L"监听目录配置（" + configPath + L"）加载完毕";
    WriteLog(wmsg, 5);

    wmsg = L"合计" + std::to_wstring(cTotal) + L"行。成功" + std::to_wstring(cSuccess) + L"行，忽略重复" + std::to_wstring(cSkipped) + L"行，跳过"
        + std::to_wstring(cIrrelevant) + L"空/注释/坏记录行，异常失败" + std::to_wstring(cFailed) + L"行（其中待重试：" + std::to_wstring(cRetry) + L"，重试间隔"
        + retryIntervalW + L"分钟），成功添加例外目录" + std::to_wstring(cExAdded) + L"个";
    WriteLog(wmsg, 5);

    // 结束时刷新一次包含file模式、current模式的清单到全局变量
    RefreshDiscardList();

}







// 正常服务启动后持久化当前状态到文件
void InitRunningCheckFile() {
    namespace fs = std::filesystem;
    // 关键检查：确保全局路径不为空
    if (g_exeRoot.empty()) {
        InitExeRootPath(); // 默认
    }

    std::wstring fileDir = g_exeRoot + L"/" + resPath;
    std::wstring filePath = fileDir + L"/" + runningCheck;

    try {
        std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
        // 写入 UTF-8 BOM 头，确保文件编码正确
        unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        ofs.write((char*)bom, 3);

        if (ofs.is_open()) {
            std::string val = std::to_string(1);
            ofs.write(val.c_str(), val.size());
            ofs.flush();
            ofs.close();
        }
    }
    catch (...) {
        // 既然已经要退出了，不再抛出异常，尝试记录一次错误日志
        std::wstring wmsg = L"保存服务运行标志位失败";
        WriteError(wmsg, g_emptyWString, 4);
    }
}

// 删除服务运行标志位文件，正常服务退出时调用
void deleteRunningCheckFile() {
    namespace fs = std::filesystem;

    if (g_exeRoot.empty()) {
        InitExeRootPath();
    }

    std::wstring fileDir = g_exeRoot + L"/" + resPath;
    std::wstring filePath = fileDir + L"/" + runningCheck;
    try {
        if (fs::exists(filePath)) {
            fs::remove(filePath);
        }
    }
    catch (...) {
        std::wstring wmsg = L"删除 服务运行标志位失败";
        WriteError(wmsg, g_emptyWString, 4);
    }
}

// 检查文件是否存在，存在则表示上次服务异常退出
bool checkRunningCheckFile() {
    namespace fs = std::filesystem;

    if (g_exeRoot.empty()) {
        InitExeRootPath();
    }
    std::wstring fileDir = g_exeRoot + L"/" + resPath;
    std::wstring filePath = fileDir + L"/" + runningCheck;
    try {
        if (!fs::exists(filePath)) {
            return false;
        }
        else {
            return true;
        }
    }
    catch (...) {
        return false;
    }
}









// std::atomic<long long> g_GlobalEventId(1000000);
// 全局计数器定义（初始值设为 0，启动后由 InitGlobalEventId 加载）
std::atomic<long long> g_GlobalEventId(0);


// 获取 EventCounter.res 的绝对路径
std::wstring GetCounterFilePath() {
    // return g_exeRoot + L"\\res\\EventCounter.res";
    return g_exeRoot + L"\\" + resPath + L"\\" + counterRES;
}
// 在服务启动阶段调用
void InitGlobalEventId() {
    namespace fs = std::filesystem;
    // 关键检查：确保全局变量不为空
    if (g_exeRoot.empty()) {
        InitExeRootPath(); // 兜底初始化
    }

    std::wstring resAbPath = g_exeRoot + L"/" + resPath;
    std::wstring filePath = GetCounterFilePath();

    try {
        // 1. 检查并创建子目录
        if (!fs::exists(resAbPath)) {
            fs::create_directories(resAbPath);
        }

        // 2. 尝试读取文件内容（使用您封装的通用解码函数）
        std::wstring content = GetFileContent(filePath);
        FormatLine(content);

        long long lastValue = 0;
        if (!content.empty()) {
            try {
                lastValue = std::stoll(content);
            }
            catch (...) {
                std::wstring wmsg = L"计数器文件格式错误，重置为初始值";
                WriteError(wmsg, g_emptyWString, 4);
            }
        }

        // 3. 初始值逻辑判断
        if (lastValue < initDefaultId) {
            g_GlobalEventId.store(initDefaultId + 1); // 情况2：新建或内容过小，设为初始值+1
        }
        else {
            g_GlobalEventId.store(lastValue + 1);; // 情况3：存在值，再加1
        }
        std::wstring wmsg = L"初始化 EventID 成功，当前值：" + std::to_wstring(g_GlobalEventId);
        WriteLog(wmsg, 5);
    }
    catch (const std::exception& e) {
        std::wstring wmsg = L"初始化 EventID 失败：" + ToWide(e.what());
        WriteError(wmsg, g_emptyWString, 4);
        g_GlobalEventId.store(initDefaultId); // 崩溃垫底值
    }
}


// 在服务退出、停止或捕获到异常时调用
void SaveGlobalEventId() {
    
    WriteLog(L"EventID持久化保存：" + std::to_wstring(g_GlobalEventId), 5);
    WriteLog(L"断开UNC连接", 5);
    WriteLog(L"========================================程序正常中止========================================", 5);
    WriteLog(g_emptyWString, 5);

    try {
        std::wstring filePath = GetCounterFilePath();
        // 以二进制写模式打开，确保 UTF-8 编码且不产生多余换行
        std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
        // 写入 UTF-8 BOM 头，确保文件编码正确
        unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        ofs.write((char*)bom, 3);

        if (ofs.is_open()) {
            std::string val = std::to_string(g_GlobalEventId.load());
            ofs.write(val.c_str(), val.size());
            ofs.flush();
            ofs.close();
        }
    }
    catch (...) {
        // 既然已经要退出了，不再抛出异常，尝试记录一次错误日志
        std::wstring wmsg = L"强制保存 EventID 失败";
        WriteError(wmsg, g_emptyWString, 4);
    }
}


// 加载时晚1秒钟触发，用于拉平注册目录上的问题，保守点把复制选项设置为3，跳过已有文件
void InitSyncDirPairOnLoad() {
    try {
        // std::this_thread::sleep_for(std::chrono::milliseconds(800));

        if (fullSyncOnLoad == 0) return;

        std::wstring pair;
        std::wstring exclude;
        std::set<std::wstring> initDest;
        for (const auto& pair : g_syncDirPair) {
            // 循环遍历所有pair，交给FileOrFolderCopyW去递归展开循环所有的，递归本地目录每个对象递归再调FileOrFolderCopyW
            // 递归到文件时会走FileOrFolderCopyW的分支处理文件，能判断出当前文件是否需要被current、exclude、single file规则所过滤
            // 所以这里只需要简单调用FileOrFolderCopyW就好
            initDest = GetAllPairedFullPathsForSync(pair.listenPath, typeFOLDER);
            FileOrFolderCopyW(pair.listenPath, initDest, true, true, fileCopyOptionOnLoad);

        }

    }
    catch (const std::filesystem::filesystem_error& e) {
        WriteFileSyncLog(L"filesystem 拷贝异常：" + ToWide(e.what()) + L"，错误码：" + std::to_wstring(e.code().value()),
            g_emptyWString, g_emptyWString, 1, 2, 4);
    }
    catch (...) {
        WriteFileSyncLog(L"filesystem 拷贝未知异常", g_emptyWString, g_emptyWString, 1, 2, 4);
    }
}



std::map<std::wstring, FileEvent> g_eventCache; // 用于防抖
std::mutex g_cacheMutex;

// 过滤规则结构
struct FilterItem {
    std::wstring type;  // start, contain, extension
    std::wstring value;
};
// 维护规则清单
std::vector<FilterItem> g_IgnoreList; // 全局过滤规则列表
std::mutex g_filterMutex;



// 加载配置的忽略过滤规则，需要在main函数启动前调用
void LoadIgnoreFilters() {
    std::wstring path = cfgPath + L"/" + filterCFG;
    std::wstring filterConfig = GetFileContent(path);
    std::wstringstream wss(filterConfig);
    std::wstring iline;
    int cSuccess = 0;
    int cFailed = 0;

    // 先定义 lock_guard，确保作用域正确
    std::lock_guard<std::mutex> lock(g_filterMutex);
    g_IgnoreList.clear();


    while (std::getline(wss, iline)) {
        // std::wstring line = FormattedLine(iline);
        std::wstring line = FormattedLine(RemoveRemarkText(iline));

        // 跳过空行、注释行
        if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;

        // ===== 格式化文本后必须以分割符开始 =====
        if (line[0] != g_delimiter) {
            ++cFailed;
            std::wstring wmsg = L"过滤规则行格式非法";
            WriteError(wmsg, line, 4);
            continue;
        }

        // ===== 2. 双引号数量必须成对 =====
        size_t quoteCount = std::count(line.begin(), line.end(), g_delimiter);
        if (quoteCount == 0 || quoteCount % 2 != 0) {
            ++cFailed;
            std::wstring wmsg = L"过滤规则行格式非法";
            WriteError(wmsg, line, 4);
            continue;
        }

        try {
            LineElements le = ReadLineElements(line, g_delimiter);
            std::wstring type = le.element1;
            std::wstring val = le.element2;

            // 预处理：如果是扩展名，统一去掉开头的点并转小写
            if (type == L"extension") {
                if (!val.empty() && val[0] == L'.') val.erase(0, 1);
                std::transform(val.begin(), val.end(), val.begin(), ::towlower);
            }
            ++cSuccess;
            g_IgnoreList.push_back({ type, val });
        }
        catch (...) {
            std::wstring wmsg = L"解析过滤规则行异常";
            WriteError(wmsg, line, 4);
        }
    }
    std::wstring wmsg = L"加载文件夹/文件名匹配规则，" + path + L"，成功加载" + std::to_wstring(cSuccess) + L"个配置项，失败" + std::to_wstring(cFailed) + L"个";
    WriteLog(wmsg, 5);
}

// 检查是否应该忽略文件，windows推送消息的文件名命中规则，则push to queue直接过滤
// 检查是否应该忽略
bool ShouldIgnore(const std::wstring& fullPath) {

    fs::path p(fullPath);
    std::wstring fileName = p.filename().wstring();              // 完整文件名 (含扩展名)
    std::wstring stemName = p.stem().wstring();                  // 文件名主体 (不含扩展名)
    std::wstring ext = p.extension().wstring();                  // 扩展名 (含点)
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);        // 去掉点方便比对

    // 统一忽略大小写：转小写
    std::wstring fileNameLower = fileName;
    std::transform(fileNameLower.begin(), fileNameLower.end(), fileNameLower.begin(), ::towlower);
    std::wstring stemNameLower = stemName;
    std::transform(stemNameLower.begin(), stemNameLower.end(), stemNameLower.begin(), ::towlower);
    std::wstring extLower = ext;
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::towlower);

    std::lock_guard<std::mutex> lock(g_filterMutex);
    for (const auto& item : g_IgnoreList) {
        std::wstring valueLower = item.value;
        std::transform(valueLower.begin(), valueLower.end(), valueLower.begin(), ::towlower);

        if (item.type == L"start") {
            if (fileNameLower.compare(0, valueLower.length(), valueLower) == 0) return true;
        }
        else if (item.type == L"contain") {
            if (fileNameLower.find(valueLower) != std::wstring::npos) return true;
        }
        else if (item.type == L"extension") {
            // 去点号（配置 value 如果以 . 开头，去除）
            if (valueLower.size() > 0 && valueLower[0] == L'.') valueLower.erase(0, 1);
            if (extLower == valueLower) return true;
        }
        // --- 新增：完整名称匹配 (含扩展名) ---
        else if (item.type == L"exact") {
            if (fileNameLower == valueLower) return true;
        }
        // --- 新增：不含扩展名的完整名称匹配 ---
        else if (item.type == L"exact-no-extension") {
            if (stemNameLower == valueLower) return true;
        }
        else if (item.type == L"end") {
            // fileName 以指定字符串结尾
            if (fileNameLower.length() >= valueLower.length() &&
                fileNameLower.compare(fileNameLower.length() - valueLower.length(), valueLower.length(), valueLower) == 0) {
                return true;
            }

            // stemName 以指定字符串结尾
            if (stemNameLower.length() >= valueLower.length() &&
                stemNameLower.compare(stemNameLower.length() - valueLower.length(), valueLower.length(), valueLower) == 0) {
                return true;
            }

            // 扩展名（小写）以指定字符串结尾
            if (extLower.length() >= valueLower.length() &&
                extLower.compare(extLower.length() - valueLower.length(), valueLower.length(), valueLower) == 0) {
                return true;
            }

            // 原始扩展名（保留大小写）以指定字符串结尾（但转小写比对）
            std::wstring extOrigLower = ext;
            std::transform(extOrigLower.begin(), extOrigLower.end(), extOrigLower.begin(), ::towlower);
            if (extOrigLower.length() >= valueLower.length() &&
                extOrigLower.compare(extOrigLower.length() - valueLower.length(), valueLower.length(), valueLower) == 0) {
                return true;
            }
        }
    }

    // ==================== Excel 临时文件特征（严格版 + 日期排除） ====================
    // 所有条件必须同时满足才返回 true（忽略该文件）
    // 条件 a-e 同原版
    // 新增排除：如果 8 位文件名全部为数字且符合日期特征（避免误杀如 "20260103"）
    // 没有扩展名或者有扩展名但转小写之后是xls或xlsx、文件名的部分（不管有没有扩展名都是指文件名本身的部分）长度固定是8位，每一位都限定只能是数字或大写A-Z，如果是全部数字的话，用现有的排除日期的逻辑保持例外防止误伤。其它的匹配规则请去掉只剩上面描述的（如果满足上述条件但有效日期格式的除外）那么就判定为true
    if (ext.empty() || extLower == L"xls" || extLower == L"xlsx") {
        // 文件名部分长度 8
        if (stemName.length() == 8) {
            // 所有字符是数字或大写 A-Z
            bool validChars = true;
            int digitCount = 0;
            for (wchar_t ch : stemName) {
                if (std::iswdigit(ch)) {
                    ++digitCount;
                }
                else if (ch < L'A' || ch > L'Z') {
                    validChars = false;
                    break;
                }
            }

            if (validChars) {
                // ==================== 保留原日期排除规则 ====================
                if (digitCount == 8) {  // 全数字时才检查日期格式（避免误杀）
                    // 全部为数字，检查日期特征
                    int year = (stemName[0] - L'0') * 1000 + (stemName[1] - L'0') * 100 +
                        (stemName[2] - L'0') * 10 + (stemName[3] - L'0');
                    int month = (stemName[4] - L'0') * 10 + (stemName[5] - L'0');
                    int day = (stemName[6] - L'0') * 10 + (stemName[7] - L'0');

                    bool firstDigitValid = (stemName[0] >= L'1' && stemName[0] <= L'9');
                    bool monthValid = (month >= 1 && month <= 12);
                    bool dayValid = (day >= 1 && day <= 31);

                    if (firstDigitValid && monthValid && dayValid) {
                        // 符合潜在日期格式（如 20260103），不视为临时文件
                        // 不返回 true，继续后续判断（避免误杀）
                        goto next_rule;
                    }
                }
                // ================================================

                // 符合条件 → 确认为 Excel 临时文件
                return true;
            }
        }
    }




next_rule:
    // ==================== 新增：~ 分割逻辑 ====================
    size_t tildePos = fileName.find(L'~');
    if (tildePos != std::wstring::npos) {
        std::wstring left = fileName.substr(0, tildePos);
        std::wstring right = fileName.substr(tildePos + 1);

        // left 含正好一个 . 
        size_t leftDotCount = std::count(left.begin(), left.end(), L'.');
        if (leftDotCount == 1) {
            size_t dotPos = left.find_last_of(L'.');
            std::wstring leftExt = left.substr(dotPos + 1);
            std::wstring leftExtLower = leftExt;
            std::transform(leftExtLower.begin(), leftExtLower.end(), leftExtLower.begin(), ::towlower);

            // 用 extension 配置匹配
            for (const auto& item : g_IgnoreList) {
                if (item.type == L"extension") {
                    std::wstring valueLower = item.value;
                    std::transform(valueLower.begin(), valueLower.end(), valueLower.begin(), ::towlower);
                    if (valueLower.size() > 0 && valueLower[0] == L'.') valueLower.erase(0, 1);
                    if (leftExtLower == valueLower) return true;
                }
            }
        }

        // right 含正好一个 .
        size_t rightDotCount = std::count(right.begin(), right.end(), L'.');
        if (rightDotCount == 1) {
            size_t dotPos = right.find_last_of(L'.');
            std::wstring rightExt = right.substr(dotPos + 1);
            std::wstring rightExtLower = rightExt;
            std::transform(rightExtLower.begin(), rightExtLower.end(), rightExtLower.begin(), ::towlower);

            // 用 extension 配置匹配
            for (const auto& item : g_IgnoreList) {
                if (item.type == L"extension") {
                    std::wstring valueLower = item.value;
                    std::transform(valueLower.begin(), valueLower.end(), valueLower.begin(), ::towlower);
                    if (valueLower.size() > 0 && valueLower[0] == L'.') valueLower.erase(0, 1);
                    if (rightExtLower == valueLower) return true;
                }
            }
        }
    }
    // ================================================

    return false;
}




void PushToQueue(const std::wstring& fullPath, const std::wstring& action, const std::wstring& eventId, const std::wstring& objType, const std::wstring& status, const int& async);
void MergeQueue(const int& callType);









// 延迟执行触发器
void SchedulerAsyncCallSelf(XYEventSchedulerContext& ctx, uint64_t execTimeMs, EventProcessType process)
{
    std::thread([&ctx, execTimeMs, process]() {
        uint64_t now = NowMs();
        if (execTimeMs > now) {
            std::this_thread::sleep_for(std::chrono::milliseconds(execTimeMs - now));
        }
        // 窗口结束回调，callType = 2
        XYEventScheduler(ctx, 2, process);
        }).detach();
}

// 统一调度器，机制是并发量很大则切分成小窗口每个窗口末触发一次判断
// 如果前一个窗口仍有请求则修正程序先跑一次，再继续开始下个窗口，中间吞掉处理请求
// 如果请求没有了则触发一次正式的后续业务程序
// 无论事件触发量的大小，既防抖、也不丢失至少一次的业务进程调用。业务处理不采用定时轮询机制
void XYEventScheduler(XYEventSchedulerContext& ctx, int callType, EventProcessType process)
{
    try {
        uint64_t now = NowMs();

        // =========================
        // ① 窗口初始化逻辑
        // =========================
        if (callType == 1 && ctx.resetFlag.load(std::memory_order_acquire) && ctx.countOtherRequests.load(std::memory_order_relaxed) > 0)
        {
            // 第一次普通请求进来、或者长时间没任何新事件后激活了这套机制，首次采用g_firstRoundTimeWindow参数，设置1秒左右
            ctx.firstTimeInWindow.store(now, std::memory_order_relaxed);
            ctx.nextTimeInWindow.store(now + ctx.firstRoundTimeWindow, std::memory_order_relaxed);
            ctx.countOtherRequests.store(1, std::memory_order_relaxed);

            // 立即处理一次，预留。暂时不发起，等下次
            //MergeQueue(1);

            // 安排窗口结束时的自调用（绑定当前 Context）
            SchedulerAsyncCallSelf(ctx, ctx.nextTimeInWindow.load(std::memory_order_relaxed), process);
            ctx.resetFlag.store(false, std::memory_order_release);
            return;
        }

        // =========================
        // ② 窗口中请求统计逻辑
        // =========================
        if (callType == 1 && !ctx.resetFlag.load(std::memory_order_acquire) && now < ctx.nextTimeInWindow.load(std::memory_order_relaxed))
        {
            ctx.countOtherRequests.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // =========================
        // ③ 窗口结束回调逻辑（callType = 2）
        // =========================
        if (callType == 2)
        {
            int otherReq = ctx.countOtherRequests.load(std::memory_order_relaxed);

            // 打印调度器特定的调用次数
            ctx.schedulerCounter.fetch_add(1, std::memory_order_relaxed);
            int scounter = ctx.schedulerCounter.load(std::memory_order_relaxed);
            WriteLog(L"服务启动期间，业务进程调度器一共被调度了：" + std::to_wstring(scounter) + L"次", 1);


            if (otherReq > 0)
            {
                // 有新请求 → 开新窗口并立刻处理
                ctx.firstTimeInWindow.store(now, std::memory_order_relaxed);
                // 由自触发产生的设置下一轮周期，通常比firstRoundTimeWindow要长因为此时一般都是大并发量为主
                ctx.nextTimeInWindow.store(now + ctx.roundTimeWindow, std::memory_order_relaxed);
                ctx.countOtherRequests.store(0, std::memory_order_relaxed);

                if (process == EventProcessType::MergeQueue) {
                    // mergequeue可以每个窗口期做一次，避免最后一次时记录过多，这是一种模式
                    MergeQueue(1);
                }
                else if (process == EventProcessType::ProcessQueue) {
                    // 预留ProcessQueue
                    // ProcessQueue(1);
                }
                else if (process == EventProcessType::RetrySyncDirPair) {
                    // RetrySyncDirPair(1);
                    // 其它只需要在没请求时再做一次批量就好的业务，每个窗口期的间隔时不需要做任何动作，只需要等待事件全部触发完，让最后一个请求数=0的窗口过完
                }
                else if (process == EventProcessType::WriteLog) {
                    // 每个窗口期写一次日志
                    WriteLogToFile();
                }
                else if (process == EventProcessType::WriteError) {
                    // 每个窗口期写一次日志
                    WriteErrorToFile();
                }
                else if (process == EventProcessType::WriteMergeLog) {
                    // 每个窗口期写一次日志
                    WriteMergeLogToFile();
                }
                else if (process == EventProcessType::WriteFileSyncLog) {
                    // 每个窗口期写一次日志
                    WriteFileSyncLogToFile();
                }
                else if (process == EventProcessType::PushQueue) {
                    // 每个窗口期写一次
                    PushQueueToFile();
                }

                // 安排下一轮窗口结束自调用
                SchedulerAsyncCallSelf(ctx, ctx.nextTimeInWindow.load(std::memory_order_relaxed), process);
            }
            else
            {
                // 无新请求 → 窗口彻底结束，并复位成初始状态
                ctx.resetFlag.store(true, std::memory_order_release);
                // 保留原先的特殊值逻辑
                ctx.countOtherRequests.store(1, std::memory_order_relaxed);
                if (process == EventProcessType::MergeQueue) {
                    // 将MQ处理完的文件一次性复制到临时文件，等待ProcessQueue处理
                    // PQ处理期间文件是立即先清空的，处理完会落成功、重试等log，并删除该临时文件
                    std::wstring lQ = g_exeRoot + L"\\" + qPath + L"\\" + mergeSuccessQ;
                    std::wstring tQ = g_exeRoot + L"\\" + qPath + L"\\" + tmpQ;
                    std::wstring fQ = g_exeRoot + L"\\" + qPath + L"\\" + fileLOGPrefix + fileRetryMiddle;
                    fs::path lFilePath = fs::absolute(lQ);
                    fs::path tFilePath = fs::absolute(tQ);
                    fs::path fFilePath = fs::absolute(fQ);
                    std::set<std::wstring> pathSet;
                    pathSet.insert(tFilePath);

                    // std::filesystem::copy_options options = std::filesystem::copy_options::overwrite_existing;
                    if (!fs::exists(tFilePath)) {
                        FileOrFolderCopyW(lFilePath, pathSet, true, true, 2);
                    }
                    // 如果文件存在且不是空文件（考虑一些特殊的BOM）设定为4字节大小
                    if (fs::exists(fFilePath) && fs::is_regular_file(fFilePath) && fs::file_size(fFilePath) > 4) ProcessQueueRetry(1);
                    // 合并彻底处理完之后触发一次Process queue
                    ProcessQueue(1);
                }
                else if (process == EventProcessType::RetrySyncDirPair) {
                    RetrySyncDirPair(1);
                }
                else if (process == EventProcessType::WriteLog) {
                    WriteLogToFile();
                }
                else if (process == EventProcessType::WriteError) {
                    WriteErrorToFile();
                }
                else if (process == EventProcessType::WriteMergeLog) {
                    WriteMergeLogToFile();
                }
                else if (process == EventProcessType::WriteFileSyncLog) {
                    WriteFileSyncLogToFile();
                }
                else if (process == EventProcessType::PushQueue) {
                    PushQueueToFile();
                }
            }
        }
    }
    catch (...) {
        WriteError(L"TryTriggerQueue 异常", g_emptyWString, 4);
    }
}


// 定义一个文件的多个相同操作时，如何按操作顺序排序，用于合并queue记录最终送给ProcessQueue执行
int GetActionOrder(const std::wstring& action, int matchGroup)
{
    if (matchGroup == 2) {
        // ADD → MOD → RENOLD → UNKNOWN
        if (action == actionADD || action == actionADDR)      return 1;
        if (action == actionMOD)      return 2;
        if (action == actionRENOLD || action == actionRENOLDR)   return 3;
        if (action == actionUNKNOWN)  return 4;
    }
    else if (matchGroup == 3) {
        // MOD → RENNEW → DEL → UNKNOWN
        if (action == actionMOD)      return 1;
        if (action == actionRENNEW || action == actionRENNEWR)   return 2;
        if (action == actionDEL || action == actionDELR)      return 3;
        if (action == actionUNKNOWN)  return 4;
    }
    else if (matchGroup == 4) {
        // RENOLD/RENOLDR 组优先 → RENNEW/RENNEWR 组 → 其他（包括 UNKNOWN）
        if (action == actionRENOLD || action == actionRENOLDR)    return 1;
        if (action == actionRENNEW || action == actionRENNEWR)    return 2;
        // 其他 action（包括 UNKNOWN、ADD、MOD、DEL 等）排在最后
        return 3;
    }
    // matchGroup = 1 或兜底
    return 100;
}

// 用于MergeQueue处理为ProcessQueue可以直接处理的记录时，查找、匹配、判定用
std::set<std::wstring> MatchQueueRowsFromSet(
    const std::set<std::wstring> inputSet,
    const std::wstring inputLine,
    const std::wstring eventId,
    const std::wstring action,
    const std::wstring objType,
    const std::wstring objName,
    const std::wstring objFullPath,
    const std::wstring status,
    const int matchGroup,
    const int excludeSelf
)
{
    std::set<std::wstring> result;

    // =========================
    // 1. 入参校验
    // =========================
    if (inputSet.empty() || inputLine.empty() || eventId.empty() || action.empty() || objType.empty() || objName.empty() || objFullPath.empty() || status.empty())
    {
        WriteError(L"MatchQueueRowsFromSet 入参不能为空", g_emptyWString, 4);
        return {};
    }

    if (matchGroup < 1 || matchGroup > 3) {
        WriteError(L"MatchQueueRowsFromSet入参matchGroup 非法", g_emptyWString, 4);
        return {};
    }

    if (excludeSelf != 0 && excludeSelf != 1) {
        WriteError(L"MatchQueueRowsFromSet入参excludeSelf 非法", g_emptyWString, 4);
        return {};
    }

    // =========================
    // 2. 校验 inputLine 本身
    // =========================
    auto selfValidate = ValidateQueueLine(inputLine);
    if (!selfValidate.valid) {
        WriteError(L"inputLine 非法: " + selfValidate.error, g_emptyWString, 4);
        return {};
    }

    QueueRow selfRow = ParseQueueLine(inputLine);

    // =========================
    // 3. 遍历匹配
    // =========================
    for (const auto& line : inputSet) {

        if (line.empty()) continue;

        auto vr = ValidateQueueLine(line);
        if (!vr.valid) continue;

        QueueRow row = ParseQueueLine(line);

        bool match = true;

        // --- 字段匹配（支持 * 通配） ---
        if (eventId != L"*" && row.eventId != eventId)           match = false;
        if (match && action != L"*" && row.action != action)     match = false;
        if (match && objType != L"*" && row.objType != objType)  match = false;
        if (match && objName != L"*" && row.objName != objName)  match = false;
        if (match && objFullPath != L"*" && row.objFullPath != objFullPath) match = false;
        if (match && status != L"*" && row.status != status)     match = false;

        if (!match) continue;

        // --- matchGroup 额外 action 限制 ---
        if (matchGroup == 2) {
            if (!(row.action == actionADD || row.action == actionADDR ||
                row.action == actionMOD ||
                row.action == actionRENOLD || row.action == actionRENOLDR ||
                row.action == actionUNKNOWN))
                continue;
        }
        else if (matchGroup == 3) {
            if (!(row.action == actionMOD ||
                row.action == actionRENNEW || row.action == actionRENNEWR ||
                row.action == actionDEL || row.action == actionDELR ||
                row.action == actionUNKNOWN))
                continue;
        }

        result.insert(line);
    }

    if (result.empty()) return {};

    // =========================
    // 4. 排序（转 vector 排）
    // =========================
    std::vector<std::wstring> sorted(result.begin(), result.end());

    std::sort(sorted.begin(), sorted.end(),
        [&](const std::wstring& a, const std::wstring& b) {

            QueueRow A = ParseQueueLine(a);
            QueueRow B = ParseQueueLine(b);

            if (A.objFullPath != B.objFullPath)
                return A.objFullPath < B.objFullPath;

            int aOrder = GetActionOrder(A.action, matchGroup);
            int bOrder = GetActionOrder(B.action, matchGroup);
            if (aOrder != bOrder)
                return aOrder < bOrder;

            if (A.timestamp != B.timestamp)
                return A.timestamp < B.timestamp;

            return A.objType < B.objType;
        });

    result.clear();
    for (const auto& l : sorted) result.insert(l);

    // =========================
    // 5. excludeSelf 处理
    // =========================
    if (excludeSelf == 1) {
        result.erase(inputLine);
    }

    return result;
}




// 用于MergeQueue处理中，按条件直接从原始set中删除匹配记录
void DeleteQueueRowsFromSet(
    std::set<std::wstring>& inputSet,
    const std::wstring inputLine,
    const std::wstring eventId,
    const std::wstring action,
    const std::wstring objType,
    const std::wstring objName,
    const std::wstring objFullPath,
    const std::wstring status,
    const int matchGroup,
    const int excludeSelf
)
{
    // =========================
    // 1. 入参校验
    // =========================
    if (inputSet.empty() || inputLine.empty() || eventId.empty() || action.empty() || objType.empty() || objName.empty() || objFullPath.empty() || status.empty())
    {
        WriteError(L"DeleteQueueRowsFromSet 入参不能为空", g_emptyWString, 4);
        return;
    }

    if (matchGroup < 1 || matchGroup > 3) {
        WriteError(L"DeleteQueueRowsFromSet 入参matchGroup 非法", g_emptyWString, 4);
        return;
    }

    if (excludeSelf != 0 && excludeSelf != 1) {
        WriteError(L"DeleteQueueRowsFromSet 入参excludeSelf 非法", g_emptyWString, 4);
        return;
    }

    // =========================
    // 2. 校验 inputLine 本身
    // =========================
    auto selfValidate = ValidateQueueLine(inputLine);
    if (!selfValidate.valid) {
        WriteError(L"inputLine 非法: " + selfValidate.error, g_emptyWString, 4);
        return;
    }

    QueueRow selfRow = ParseQueueLine(inputLine);

    // =========================
    // 3. 遍历匹配
    // =========================
    std::vector<std::wstring> toDelete;

    for (const auto& line : inputSet) {

        if (line.empty()) continue;

        auto vr = ValidateQueueLine(line);
        if (!vr.valid) continue;

        QueueRow row = ParseQueueLine(line);

        bool match = true;

        // --- 字段匹配（支持 * 通配） ---
        if (eventId != L"*" && row.eventId != eventId)           match = false;
        if (match && action != L"*" && row.action != action)     match = false;
        if (match && objType != L"*" && row.objType != objType)  match = false;
        if (match && objName != L"*" && row.objName != objName)  match = false;
        if (match && objFullPath != L"*" && row.objFullPath != objFullPath) match = false;
        if (match && status != L"*" && row.status != status)     match = false;

        if (!match) continue;

        // --- matchGroup 额外 action 限制 ---
        if (matchGroup == 2) {
            if (!(row.action == actionADD || row.action == actionADDR ||
                row.action == actionMOD ||
                row.action == actionRENOLD || row.action == actionRENOLDR ||
                row.action == actionUNKNOWN))
                continue;
        }
        else if (matchGroup == 3) {
            if (!(row.action == actionMOD ||
                row.action == actionRENNEW || row.action == actionRENNEWR ||
                row.action == actionDEL || row.action == actionDELR ||
                row.action == actionUNKNOWN))
                continue;
        }

        toDelete.push_back(line);
    }

    if (toDelete.empty()) return;

    // =========================
    // 4. excludeSelf 处理，排除自身在这里的意思是自身不删除，因此=1要排除时，从匹配的列表里删除掉，后续在第5块删除操作时就不会删了自身
    // =========================
    if (excludeSelf == 1) {
        auto it = std::find(toDelete.begin(), toDelete.end(), inputLine);
        if (it != toDelete.end()) {
            toDelete.erase(it);
        }
    }

    // =========================
    // 5. 从 inputSet 中删除匹配记录
    // =========================
    for (const auto& l : toDelete) {
        inputSet.erase(l);
    }
}





// queue记录的单独业务字段输入，拼装为一行wstring，供PushQ写入原始监听文件
std::wstring QueueFieldsToLine(const std::wstring& action, const std::wstring& objType, const std::wstring& fullPath,
    const std::wstring& timestamp, const std::wstring& eventId, const std::wstring& status) {

    // 原子方法不作校验，忠实按输入拼装
    std::wstring sts = FormattedLine(status).empty() ? aStatusINIT : FormattedLine(status);
    std::wstring fileName = fs::path(fullPath).filename().wstring();

    std::wstring ftimestamp = FormattedLine(timestamp).empty() ? ToWide(GetCurrentTimestamp()) : FormattedLine(timestamp);

    std::wstring raction = RemoveWLinebreak(action);
    std::wstring robjtype = RemoveWLinebreak(objType);
    std::wstring rfilename = RemoveWLinebreak(fileName);
    std::wstring rfullpath = RemoveWLinebreak(fullPath);
    std::wstring rtimestamp = RemoveWLinebreak(ftimestamp);
    std::wstring reventid = RemoveWLinebreak(eventId);
    std::wstring rstatus = RemoveWLinebreak(status);

    std::wstringstream line;
    line << L"\"" << raction << L"\"    "
        << L"\"" << robjtype << L"\"    "
        << L"\"" << rfilename << L"\"    "
        << L"\"" << rfullpath << L"\"    "
        << L"\"" << rtimestamp << L"\"    "
        << L"\"" << reventid << L"\"    "
        << L"\"" << rstatus << L"\"    ";
    return line.str();
}

// 将 QueueRow 对象转换为 queue 文件的一行 wstring
std::wstring QueueRowToQueueLine(const QueueRow& row) {

    std::wstring sts = row.status.empty() ? aStatusINIT : row.status;
    std::wstring rtimestamp = row.timestamp.empty() ? ToWide(GetCurrentTimestamp()) : row.timestamp;
    // 处理每个字段，去掉换行
    std::wstring raction = RemoveWLinebreak(row.action);
    std::wstring robjtype = RemoveWLinebreak(row.objType);
    std::wstring rfilename = RemoveWLinebreak(row.objName);
    std::wstring rfullpath = RemoveWLinebreak(row.objFullPath);
    rtimestamp = RemoveWLinebreak(rtimestamp);
    std::wstring reventid = RemoveWLinebreak(row.eventId);
    std::wstring rstatus = RemoveWLinebreak(row.status);

    std::wstringstream line;
    line << L"\"" << raction << L"\"    "
        << L"\"" << robjtype << L"\"    "
        << L"\"" << rfilename << L"\"    "
        << L"\"" << rfullpath << L"\"    "
        << L"\"" << rtimestamp << L"\"    "
        << L"\"" << reventid << L"\"    "
        << L"\"" << rstatus << L"\"    ";

    return line.str();
}


// 写入消息队列，被调用方法仅过缓存，触发调度器
void PushToQueue(const std::wstring& fullPath, const std::wstring& action, const std::wstring& eventId, const std::wstring& objType, const std::wstring& status, const int& async)
{
    std::wstring inputData = QueueFieldsToLine(action, objType, fullPath, g_emptyWString, eventId, status);
    g_queueLines.push_back(inputData);
    XYEventScheduler(PushQueueCtx, 1, EventProcessType::PushQueue);
}
// PushQ缓存写文件，由调度器调度
void PushQueueToFile()
{
    if (g_queueLines.empty()) return;
    try {
        fs::create_directories(qPath);
    }
    catch (const std::exception& e) {
        std::wstring wmsg = L"创建目录失败：" + ToWide(e.what());
        WriteError(wmsg, g_emptyWString, 4);
    }
    std::wstring queuePath = qPath + L"/" + listenQ;

    if (g_debugModeOnPushQueue) {
        logCurrentAllLoadedFolders();
        logCurrentLoadedExcludedFolders();
        logCurrentDirPairs(SYNC_DIR_PAIR);
        logCurrentDirPairs(SYNC_DIR_PAIR_RETRY);
    }

    if (!g_queueLines.empty()) {
        WriteUTF8LinesToFile(queuePath, &g_queueMutex, g_queueLines, 2);
        g_queueLines.clear();
        XYEventScheduler(MergeQueueCtx, 1, EventProcessType::MergeQueue);
    }
}


void WriteDebugExceptionHex(
    const std::string& exceptionType,
    const std::string& message,
    const std::string& location = "unknown"
) {
    try {
        fs::create_directories("log");
        std::ofstream log("log/debug-err.log", std::ios::app | std::ios::binary);
        if (!log.is_open()) return;

        // 当前时间
        std::time_t t = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%c", std::localtime(&t));

        std::lock_guard<std::mutex> lock(debugLogMutex);

        log << "[" << buf << "] "
            << "Location: " << location << " | "
            << "Exception Type: " << exceptionType << " | "
            << "Message: " << message << "\n";

        // 打印原始输入 HEX
        log << "Original input HEX: ";
        for (unsigned char c : message) {
            log << std::hex << std::setw(2) << std::setfill('0') << (int)c << " ";
        }
        log << "\n\n";
    }
    catch (...) {
        // 防止日志写入失败导致服务崩溃
    }
}







// 单独的队列重试处理，机制就是简单把retry queue里的记录搬到ready queue里，再单独触发一次ProcessQueue，至少哪里发起这个重试另处理
void ProcessQueueRetry(const int& callType) {
    if (callType != 1 && callType != 2) {
        std::wstring wmsg = L"ProcessQueue调用参数非法，callType必须为1或2";

        WriteFileSyncLog(wmsg, g_emptyWString, g_emptyWString, 5, 2, 4);
        return;
    }

    std::wstring mergeSQ = qPath + L"/" + mergeSuccessQ;
    std::wstring fileRQ = qPath + L"/" + fileLOGPrefix + fileRetryMiddle;
    std::set<std::wstring> linesMove;
    std::set<std::wstring> linesListen;
    std::wstring line;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);

        if (!fs::exists(qPath)) fs::create_directories(qPath);
        if (!fs::exists(mergeSQ)) {
            std::ofstream outfile(mergeSQ); outfile.flush(); outfile.close();
        }
        linesMove = FileToWStringSetXY(ToUTF8(fileRQ));
        std::set<std::wstring> emptyLine;
        emptyLine.insert(L"\r\n");
        WriteUTF8LinesToFile(fileRQ, nullptr, emptyLine, 1);


        for (const auto& line : linesMove) {
            if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;
            linesListen.insert(line);
        }

        WriteUTF8LinesToFile(mergeSQ, nullptr, linesListen, 2);
    }
    ProcessQueue(1);
}


// callType：1 - 由原始windows触发文件变更+防抖+office类防抖调用，2 - 由Merge自身特定场景重试发起时调用。异步延时不同
void ProcessQueue(const int& callType) {
    if (callType != 1 && callType != 2) {
        std::wstring wmsg = L"ProcessQueue调用参数非法，callType必须为1或2";

        WriteFileSyncLog(wmsg, g_emptyWString, g_emptyWString, 5, 2, 4);
        return;
    }

    // 以防万一在同步操作前再开启一次会话，如果已经开启则在方法内会跳过
    StartUNCSession();

    std::wstring tempQ = qPath + L"/" + tmpQ;
    std::wstring mergeSQ = qPath + L"/" + mergeSuccessQ;
    std::wstring fileSQ = qPath + L"/" + fileLOGPrefix + fileSuccessMiddle;
    std::wstring fileRQ = qPath + L"/" + fileLOGPrefix + fileRetryMiddle;
    std::wstring fileFQ = qPath + L"/" + fileLOGPrefix + fileFailedMiddle;
    std::wstring fileDQ = qPath + L"/" + fileLOGPrefix + fileDiscardedMiddle;
    std::set<std::wstring> linesReady; // 输入，merge成功合并记录

    // 单独区块读完文件，一次性读一次性释放，读完之后用覆盖模式写一行空行，交还给MergeQueue
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);

        if (!fs::exists(qPath)) fs::create_directories(qPath);
        if (!fs::exists(fileSQ)) {
            std::ofstream outfile(fileSQ); outfile.flush(); outfile.close();
        }
        linesReady = FileToWStringSetXY(ToUTF8(mergeSQ));
        std::set<std::wstring> emptyLine;
        emptyLine.insert(L"\r\n");
        WriteUTF8LinesToFile(mergeSQ, nullptr, emptyLine, 1);
    }

    // 准备set容器
    std::set<std::wstring> linesProcessed; // 主体成功记录
    std::set<std::wstring> linesProcessedSizeAdded; // 最后更新一次文件大小信息
    std::set<std::wstring> linesFailed; // 失败记录，只作日志记录
    std::set<std::wstring> linesRetry; // 待重试清单，一方面写日志，另一方面重送队列（待考虑用文件mergePassedQ还是直接推进最初listen队列）


    std::set<std::wstring> linesDiscarded;


    // 第一遍先做文件夹增删两个set的递归根操作，目录只保留递归根、有几个保留几个，其次是对应的文件记录也全部删除
    QueueSet qs = ParseQueueSet(linesReady);
    // 递归根的操作只能在这里进行，因为MergeQueue是分批次，只有ProcessQueue才能看到一批操作完整的所有记录
    RemoveRecursiveFoldersForQueueSet(qs, 1);
    RemoveRecursiveFoldersForQueueSet(qs, 2);

    // 跨操作类型时序控制标志：当新增记录有关联的重命名或删除记录时，后续操作需等待以确保远程文件系统落盘
    bool addHasLinkedRename = false;   // 新增记录有关联的 RENAME_OLD → otherSet 循环前等待
    bool addHasLinkedDelete = false;   // 新增记录有关联的 DEL → deleteSet 循环前等待
    bool deleteHasLinkedRename = false; // 删除记录有关联的 RENAME_NEW → otherSet 循环前等待（删除后重命名场景）

    xyFileObject srcFile;
    xyFileObject destFile;

    std::wstring updatedLine;
    std::wstring updatedMatchedLine;
    std::set<std::wstring> criteria;
    QueueRow matchedRow;
    std::set<std::wstring> destFullPath;
    std::set<std::pair<std::wstring, std::wstring>> moveMultiTargets;
    bool recursive = true;
    int validateDest;

    {
        try {

            // 处理顺序的核心是，文件夹先新增，然后是所有的文件部分（所有操作），然后是文件夹的删除
            // 处理时需要考虑合并后的类型如ADD、ADDR、DEL、DELR，因此集合之间需要关联查询
            // 文件夹ADD是纯新增，ADDR一定会有一个关联的RENAME NEW；DELR一定会有一个关联的RENAME OLD
            // 这里的集合是递归根之后的结果，队列记录已经大大简化，递归根处理之后基本等同于用户原始文件夹复制、删除操作


            // 创建、创建+重命名目录（从浅到深）
            auto queSubset = GetSortedFolderListFromQueueSet(qs, 1);
            for (const auto& qr : queSubset) {

                // 预处理，由于single、current模式导致要丢弃的文件，在多目标情况下如果仍无匹配，则跳过
                int countTargets = countValidTargets(qr.objFullPath, qr.objType);
                if (countTargets == 0) {
                    linesDiscarded.insert(qr.originalLine);
                    continue;
                }

                if (qr.action == actionADD || qr.action == actionADDR) {

                    // 处理文件夹剪切的情况，取到记录并删除folderDeleteSet，在这里一并处理
                    if (qr.action == actionADD) {
                        criteria.insert(actionDEL);
                        matchedRow = DeleteQueueRowFromQueueSetByNameAAction(qs.folderDeleteSet, qr.objName, qr.timestamp, qr.objFullPath, criteria);
                        criteria.clear();
                        if (!matchedRow.empty()) moveMultiTargets = GetPairedFullPathPairForMoveSync(matchedRow.objFullPath, qr.objFullPath, qr.objType);
                    }


                    // 文件夹新增后有重命名，那成对的要么是RENNEW未变更过，或者RENNEWR变更过
                    // RENNEWR只会在文件上有，文件夹一般不会修改过。成对的也不会是delete因为对冲过了
                    if (qr.action == actionADDR) {
                        criteria.insert(actionRENNEW);
                        criteria.insert(actionRENNEWR);
                        matchedRow = DeleteQueueRowFromQueueSetByEventAAction(qs.otherSet, qr.eventId, criteria);
                        criteria.clear();
                        destFullPath = GetAllPairedFullPathsForSync(matchedRow.objFullPath, matchedRow.objType);
                    }
                    else {
                        destFullPath = GetAllPairedFullPathsForSync(qr.objFullPath, qr.objType);
                    }

                    validateDest = DirectoryExistsWithCredential2(destFullPath, 2);
                    // 操作前验证路径是否可访问，0继续，-3入重试，-1是不存在要看场景来处理，其它入失败
                    if (validateDest == -3) {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPRETRY);
                        if (qr.action == actionADDR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPRETRY);
                        linesRetry.insert(updatedLine);
                        if (qr.action == actionADDR) linesRetry.insert(updatedMatchedLine);
                        continue;
                    }
                    else if (validateDest == 0) {
                        // 成功则什么都不做
                    }
                    else {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                        if (qr.action == actionADDR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                        linesFailed.insert(updatedLine);
                        if (qr.action == actionADDR) linesFailed.insert(updatedMatchedLine);
                        continue;
                    }

                    if (!moveMultiTargets.empty()) {
                        FileOrFolderMove(moveMultiTargets); // 剪切类操作合并处理ADD和DEL，适配多目标。适用于前切前后均在监听目录范围，否则走普通新增/删除逻辑
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                        linesProcessed.insert(updatedLine);
                        updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPSUCCESS);
                        linesProcessed.insert(updatedMatchedLine);
                        continue;
                    }
                    else {
                        if (qr.action == actionADDR) {
                            FileOrFolderCopy(matchedRow.objFullPath, destFullPath, true, recursive, fileCopyOption);
                        }
                        else {
                            FileOrFolderCopy(qr.objFullPath, destFullPath, true, recursive, fileCopyOption);
                        }
                    }


                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                    linesProcessed.insert(updatedLine);
                    if (qr.action == actionADDR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPSUCCESS); // RENNEW/R那条记录
                    if (qr.action == actionADDR) linesProcessed.insert(updatedMatchedLine);

                    // 时序控制：检测 ADD 记录是否有关联的重命名或删除记录（跨 eventId 按路径匹配）
                    if (qr.action == actionADD) {
                        std::set<std::wstring> renOldCriteria = { actionRENOLD };
                        if (!FindQueueRowFromQueueSetByFullPathAAction(qs.otherSet, qr.objFullPath, renOldCriteria).empty()) {
                            addHasLinkedRename = true;
                        }
                        std::set<std::wstring> delCriteria = { actionDEL };
                        if (!FindQueueRowFromQueueSetByFullPathAAction(qs.folderDeleteSet, qr.objFullPath, delCriteria).empty() ||
                            !FindQueueRowFromQueueSetByFullPathAAction(qs.fileDeleteSet, qr.objFullPath, delCriteria).empty()) {
                            addHasLinkedDelete = true;
                        }
                    }

                    // WriteFileSyncLog(L"目录创建成功", g_emptyWString, destFullPath, 1, 1, 2);
                    continue;
                }
                else {

                    // 这里是针对ADD和ADDR外的记录，万一有则归入UNKNOWN进失败日志
                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPUNKNOWN);
                    linesFailed.insert(updatedLine);
                    WriteFileSyncLog(L"folderAddSet里存在未识别场景数据，归入失败待分析", qr.objFullPath, g_emptyWString, 1, 2, 4); // 新增，失败，错误。业务定义的异常
                    continue;
                }

            }


            // 新增和新增+重命名文件
            std::set<QueueRow> queSubrow = qs.fileAddSet;
            for (const auto& qr : queSubrow) {

                // 预处理，由于single、current模式导致要丢弃的文件，在多目标情况下如果仍无匹配，则跳过
                int countTargets = countValidTargets(qr.objFullPath, qr.objType);
                if (countTargets == 0) {
                    linesDiscarded.insert(qr.originalLine);
                    continue;
                }

                if (qr.action == actionADD || qr.action == actionADDR) {

                    // 处理文件剪切的情况，取到记录并删除fileDeleteSet，在这里一并处理
                    if (qr.action == actionADD) {
                        criteria.insert(actionDEL);
                        matchedRow = DeleteQueueRowFromQueueSetByNameAAction(qs.fileDeleteSet, qr.objName, qr.timestamp, qr.objFullPath, criteria);
                        criteria.clear();
                        if (!matchedRow.empty()) moveMultiTargets = GetPairedFullPathPairForMoveSync(matchedRow.objFullPath, qr.objFullPath, qr.objType);
                    }

                    // 通用的ADDR和RENAME NEW/NEW_R组合
                    if (qr.action == actionADDR) {
                        criteria.insert(actionRENNEW);
                        criteria.insert(actionRENNEWR);
                        matchedRow = DeleteQueueRowFromQueueSetByEventAAction(qs.otherSet, qr.eventId, criteria);
                        criteria.clear();
                        destFullPath = GetAllPairedFullPathsForSync(matchedRow.objFullPath, matchedRow.objType);
                    }
                    else {
                        destFullPath = GetAllPairedFullPathsForSync(qr.objFullPath, qr.objType);
                    }

                    // 非常特殊的文件更新机制，如snipaste配置文件更新方式：先删除常规ini、再新增临时文件、再把临时文件命名回常规ini。按通用merge后结果基础处理
                    // 注意：仅适用于这组文件没有因单文件、current被过滤掉情况时，如果matchedRow被过滤掉，那么只剩DELR记录，需要走DELR里的特殊逻辑
                    bool delAndRenameBack = false;
                    QueueRow snipasteMatchedRow;  // snipaste场景独立变量，避免覆盖 RENAME_NEW 的 matchedRow
                    QueueRow snipasteLockRows;
                    if (qr.action == actionADDR) {
                        criteria.insert(actionDELR);
                        snipasteMatchedRow = DeleteQueueRowFromQueueSetByEventAAction(qs.fileDeleteSet, qr.eventId, criteria);
                        criteria.clear();
                        // 例子：ADDR: config.ini.rBPjng；DELR: config.ini，处理为按DELR的路径直接更新文件
                        if (!snipasteMatchedRow.empty() && qr.objName != snipasteMatchedRow.objName && ContainStringOrEqual(qr.objName, snipasteMatchedRow.objName)) {
                            destFullPath = GetAllPairedFullPathsForSync(snipasteMatchedRow.objFullPath, snipasteMatchedRow.objType);
                            delAndRenameBack = true;
                        }
                        // 特殊lock文件也一并删除
                        std::wstring snipasteLockFile = snipasteMatchedRow.objName + L".lock";
                        criteria.insert(actionDEL);
                        snipasteLockRows = DeleteQueueRowFromQueueSetByNameAAction(qs.fileDeleteSet, snipasteLockFile, snipasteMatchedRow.timestamp, snipasteMatchedRow.objFullPath, criteria);
                        criteria.clear();
                    }



                    validateDest = DirectoryExistsWithCredential2(destFullPath, 2);
                    // 操作前验证路径是否可访问，0继续，-3入重试，-1是不存在要看场景来处理，其它入失败
                    if (validateDest == -3) {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPRETRY);
                        if (qr.action == actionADDR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPRETRY);
                        linesRetry.insert(updatedLine);
                        if (qr.action == actionADDR) linesRetry.insert(updatedMatchedLine);
                        continue;
                    }
                    else if (validateDest == 0) {
                        // 成功则什么都不做
                    }
                    else if (validateDest == -5) {
                        // 新增或新增+重命名文件时如果目录不存在，先创建目录链
                        for (const auto& singleDest : destFullPath) {
                            try {
                                std::filesystem::create_directories(std::filesystem::path(GetParentPathNoBackSlash(singleDest)));
                                WriteFileSyncLog(L"目标目录不存在，已自动创建：" + GetParentPathNoBackSlash(singleDest), g_emptyWString, g_emptyWString, 1, 1, 2);
                            }
                            catch (const std::exception& e) {
                                WriteFileSyncLog(L"自动创建目录失败：" + ToWide(e.what()), GetParentPathNoBackSlash(singleDest), g_emptyWString, 1, 2, 4);
                            }
                        }

                    }
                    else {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                        if (qr.action == actionADDR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                        linesFailed.insert(updatedLine);
                        if (qr.action == actionADDR) linesFailed.insert(updatedMatchedLine);
                        continue;
                    }

                    for (const auto& singleDest : destFullPath) {
                        // 如果目标文件存在、文件满足移除锁场景则移除锁，如和移除锁冲突则进失败队列，在os异常之前拦截掉；
                        if (std::filesystem::exists(singleDest)) {
                            srcFile = qr.action == actionADDR ? GetFileObjectAttr(matchedRow.objFullPath) : GetFileObjectAttr(qr.objFullPath);
                            destFile = GetFileObjectAttr(singleDest);
                            if (destFile.isReadOnly && (fileCopyOption == 2 || fileCopyOption == 1) && CompareFileTime(&srcFile.lastWriteTime, &destFile.lastWriteTime) > 0) {
                                if (fileCopyRemoveLock) {
                                    FileRemoveReadOnly(singleDest);
                                }
                                // 目标文件只读、源文件比目标文件新（意思是想要覆盖）、系统开关1或2都是要更新情况下，进入失败分支
                                else {
                                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                                    if (qr.action == actionADDR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                                    linesFailed.insert(updatedLine);
                                    if (qr.action == actionADDR) linesFailed.insert(updatedMatchedLine);
                                    WriteFileSyncLog(L"目标文件被锁定，fileCopyRemoveLock禁用，源文件较新，OS异常前拦截", g_emptyWString, singleDest, 1, 2, 4); // 新增，失败，错误。业务定义的异常
                                    continue;
                                }
                            }
                        }
                    }

                    if (!moveMultiTargets.empty()) {
                        FileOrFolderMove(moveMultiTargets); // 剪切类操作合并处理ADD和DEL，适配多目标。适用于前切前后均在监听目录范围，否则走普通新增/删除逻辑
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                        linesProcessed.insert(updatedLine);
                        updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPSUCCESS);
                        linesProcessed.insert(updatedMatchedLine);
                        continue;
                    }
                    // snipaste类文件先删再新增临时文件再命名回原始被删除文件这类模式，按删除文件的路径仅按文件更新处理
                    else if (delAndRenameBack) {
                        FileOrFolderCopyW(snipasteMatchedRow.objFullPath, destFullPath, true, recursive, fileCopyOption);
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                        updatedLine = UpdateLineElementByIndex(updatedLine, 1, actionDELR);
                        linesProcessed.insert(updatedLine);
                        updatedMatchedLine = UpdateLineElementByIndex(snipasteMatchedRow.originalLine, 7, aStatusPSUCCESS);
                        updatedMatchedLine = UpdateLineElementByIndex(updatedMatchedLine, 1, actionMOD);
                        linesProcessed.insert(updatedMatchedLine);
                        updatedMatchedLine = UpdateLineElementByIndex(snipasteLockRows.originalLine, 7, aStatusPSUCCESS);
                        updatedMatchedLine = UpdateLineElementByIndex(updatedMatchedLine, 1, actionDEL);
                        linesProcessed.insert(updatedMatchedLine);
                        continue;
                    }
                    else {
                        if (qr.action == actionADDR) {
                            FileOrFolderCopy(matchedRow.objFullPath, destFullPath, true, recursive, fileCopyOption);
                        }
                        else {
                            FileOrFolderCopy(qr.objFullPath, destFullPath, true, recursive, fileCopyOption);
                        }
                    }

                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                    if (qr.action == actionADDR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPSUCCESS);
                    linesProcessed.insert(updatedLine);
                    if (qr.action == actionADDR) linesProcessed.insert(updatedMatchedLine);

                    // 时序控制：检测 ADD 记录是否有关联的重命名或删除记录
                    if (qr.action == actionADD) {
                        std::set<std::wstring> renOldCriteria = { actionRENOLD };
                        if (!FindQueueRowFromQueueSetByFullPathAAction(qs.otherSet, qr.objFullPath, renOldCriteria).empty()) {
                            addHasLinkedRename = true;
                        }
                        std::set<std::wstring> delCriteria = { actionDEL };
                        if (!FindQueueRowFromQueueSetByFullPathAAction(qs.folderDeleteSet, qr.objFullPath, delCriteria).empty() ||
                            !FindQueueRowFromQueueSetByFullPathAAction(qs.fileDeleteSet, qr.objFullPath, delCriteria).empty()) {
                            addHasLinkedDelete = true;
                        }
                    }

                    // WriteFileSyncLog(L"文件同步成功", g_emptyWString, destFullPath, 1, 1, 2);
                    continue;
                }
                else {
                    // 这里是针对ADD和ADDR外的记录，万一有则归入UNKNOWN进失败日志
                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPUNKNOWN);
                    linesFailed.insert(updatedLine);
                    WriteFileSyncLog(L"fileAddSet里存在未识别场景数据，归入失败待分析", qr.objFullPath, g_emptyWString, 1, 2, 4); // 新增，失败，错误。业务定义的异常
                    continue;
                }

            }


            // 文件的删除部分比other先处理，因为处理删除时，删除+重命名组合的重命名部分可以先被从集合里删除，后面处理就干净了
            // 文件的删除和重命名+删除部分
            // 文件删除时，循环记录是del这条，如果是DELR，那么必定有重命名OLD的一条匹配，删除时从目标端要去找改名前的旧命名
            queSubrow = qs.fileDeleteSet;
            for (const auto& qr : queSubrow) {

                // 预处理，由于single、current模式导致要丢弃的文件，在多目标情况下如果仍无匹配，则跳过
                int countTargets = countValidTargets(qr.objFullPath, qr.objType);
                if (countTargets == 0) {
                    linesDiscarded.insert(qr.originalLine);
                    continue;
                }

                if (qr.action == actionDEL || qr.action == actionDELR) {
                    bool delAndRenameBack = false;
                    if (qr.action == actionDELR) {

                        // snipaste类 + 临时文件被单文件、current模式跳过时的特殊处理
                        criteria.insert(actionADDR);
                        matchedRow = DeleteQueueRowFromQueueSetByEventAAction(qs.fileAddSet, qr.eventId, criteria);
                        criteria.clear();
                        if (!matchedRow.empty() && ContainStringOrEqual(matchedRow.objName, qr.objName) && matchedRow.objName != qr.objName) {
                            destFullPath = GetAllPairedFullPathsForSync(qr.objFullPath, qr.objType);
                            delAndRenameBack = true;
                        }
                        // 普通的DELR找RENAME OLD
                        else {
                            criteria.insert(actionRENOLD);
                            criteria.insert(actionRENOLDR);
                            matchedRow = DeleteQueueRowFromQueueSetByEventAAction(qs.otherSet, qr.eventId, criteria);
                            criteria.clear();
                            destFullPath = GetAllPairedFullPathsForSync(matchedRow.objFullPath, matchedRow.objType);

                        }

                    }
                    else {
                        destFullPath = GetAllPairedFullPathsForSync(qr.objFullPath, qr.objType);
                    }

                    validateDest = DirectoryExistsWithCredential2(destFullPath, 2);
                    // 操作前验证路径是否可访问，0继续，-3入重试，-1是不存在要看场景来处理，其它入失败
                    if (validateDest == -3) {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPRETRY);
                        if (qr.action == actionDELR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPRETRY);
                        linesRetry.insert(updatedLine);
                        if (qr.action == actionDELR) linesRetry.insert(updatedMatchedLine);
                        continue;
                    }
                    else if (validateDest == 0) {
                        // 成功则什么都不做
                    }
                    else {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                        if (qr.action == actionDELR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                        linesFailed.insert(updatedLine);
                        if (qr.action == actionDELR) linesFailed.insert(updatedMatchedLine);
                        continue;
                    }

                    // 删除部分没有文件锁定的讲究，跳过对文件锁定的处理，snipaste类特殊情况除外，因为本质是更新文件
                    if (delAndRenameBack) {
                        for (const auto& singleDest : destFullPath) {
                            // 如果目标文件存在、文件满足移除锁场景则移除锁，如和移除锁冲突则进失败队列，在os异常之前拦截掉；
                            if (std::filesystem::exists(singleDest)) {
                                srcFile = qr.action == actionADDR ? GetFileObjectAttr(matchedRow.objFullPath) : GetFileObjectAttr(qr.objFullPath);
                                destFile = GetFileObjectAttr(singleDest);
                                if (destFile.isReadOnly && (fileCopyOption == 2 || fileCopyOption == 1) && CompareFileTime(&srcFile.lastWriteTime, &destFile.lastWriteTime) > 0) {
                                    if (fileCopyRemoveLock) {
                                        FileRemoveReadOnly(singleDest);
                                    }
                                    // 目标文件只读、源文件比目标文件新（意思是想要覆盖）、系统开关1或2都是要更新情况下，进入失败分支
                                    else {
                                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                                        if (qr.action == actionADDR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                                        linesFailed.insert(updatedLine);
                                        if (qr.action == actionADDR) linesFailed.insert(updatedMatchedLine);
                                        WriteFileSyncLog(L"目标文件被锁定，fileCopyRemoveLock禁用，源文件较新，OS异常前拦截", g_emptyWString, singleDest, 1, 2, 4); // 新增，失败，错误。业务定义的异常
                                        continue;
                                    }
                                }
                            }
                        }
                    }


                    if (delAndRenameBack) {
                        if (addHasLinkedDelete) {
                            auto copyDestFullPath = destFullPath;
                            auto copySrcFullPath = qr.objFullPath;
							auto copyFileCopyOption = fileCopyOption;
                            std::thread([copyDestFullPath, copySrcFullPath, recursive, copyFileCopyOption]() {
                                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                                FileOrFolderCopyW(copySrcFullPath, copyDestFullPath, true, recursive, copyFileCopyOption);
                            }).detach();
                        }
                        else {
                            FileOrFolderCopyW(qr.objFullPath, destFullPath, true, recursive, fileCopyOption);
                        }
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                        updatedLine = UpdateLineElementByIndex(updatedLine, 1, actionMOD);
                        linesProcessed.insert(updatedLine);
                        continue;
                    }
                    else {
                        // destFullPath同时包含了改名或未改名情况
                        if (addHasLinkedDelete) {
                            auto copyDestFullPath = destFullPath;
                            std::thread([copyDestFullPath]() {
                                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                                FileOrFolderDelete(copyDestFullPath, true);
                            }).detach();
                        }
                        else {
                            FileOrFolderDelete(destFullPath, true);
                        }
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                        if (qr.action == actionDELR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPSUCCESS);
                        linesProcessed.insert(updatedLine);
                        if (qr.action == actionDELR) linesProcessed.insert(updatedMatchedLine);

                        // 时序控制：检测 DEL 记录是否有关联的重命名记录
                        if (qr.action == actionDEL) {
                            std::set<std::wstring> renNewCriteria = { actionRENNEW };
                            if (!FindQueueRowFromQueueSetByFullPathAAction(qs.otherSet, qr.objFullPath, renNewCriteria).empty()) {
                                deleteHasLinkedRename = true;
                            }
                        }

                        // WriteFileSyncLog(L"文件同步成功", g_emptyWString, destFullPath, 1, 1, 2);
                        continue;
                    }

                }
                else {
                    // 这里是针对DEL和DELR外的记录，万一有则归入UNKNOWN进失败日志
                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPUNKNOWN);
                    linesFailed.insert(updatedLine);
                    WriteFileSyncLog(L"fileDeleteSet里存在未识别场景数据，归入失败待分析", qr.objFullPath, g_emptyWString, 1, 2, 4); // 新增，失败，错误。业务定义的异常
                    continue;
                }

            }



            // 接着处理文件夹删除
            // 前提：没有纯净、单独的文件的修改、重命名是依赖这些被删除文件夹的，否则就会目录被先删，轮到fileOtherSet剩余记录时报错
            // 短时间内的操作理论上没有，后面观察。如果网络异常重试，此时时间过了一段时间，积累下来没被处理掉的记录可能会有

            queSubset = GetSortedFolderListFromQueueSet(qs, 2);
            for (const auto& qr : queSubset) {

                // 预处理，由于single、current模式导致要丢弃的文件，在多目标情况下如果仍无匹配，则跳过
                int countTargets = countValidTargets(qr.objFullPath, qr.objType);
                if (countTargets == 0) {
                    linesDiscarded.insert(qr.originalLine);
                    continue;
                }

                if (qr.action == actionDEL || qr.action == actionDELR) {

                    if (qr.action == actionDELR) {
                        criteria.insert(actionRENOLD);
                        criteria.insert(actionRENOLDR);
                        matchedRow = DeleteQueueRowFromQueueSetByEventAAction(qs.otherSet, qr.eventId, criteria);
                        criteria.clear();
                        destFullPath = GetAllPairedFullPathsForSync(matchedRow.objFullPath, matchedRow.objType);
                    }
                    else {
                        destFullPath = GetAllPairedFullPathsForSync(qr.objFullPath, qr.objType);
                    }

                    validateDest = DirectoryExistsWithCredential2(destFullPath, 2);
                    // 操作前验证路径是否可访问，0继续，-3入重试，-1是不存在要看场景来处理，其它入失败
                    if (validateDest == -3) {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPRETRY);
                        if (qr.action == actionDELR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPRETRY);
                        linesRetry.insert(updatedLine);
                        if (qr.action == actionDELR) linesRetry.insert(updatedMatchedLine);
                        continue;
                    }
                    else if (validateDest == 0) {
                        // 成功则什么都不做
                    }
                    else {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                        if (qr.action == actionDELR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                        linesFailed.insert(updatedLine);
                        if (qr.action == actionDELR) linesFailed.insert(updatedMatchedLine);
                        continue;
                    }

                    // 删除部分没有文件锁定的讲究，跳过对文件锁定的处理

                    // destFullPath同时包含了改名或未改名情况
                    if (addHasLinkedDelete) {
                        auto copyDestFullPath = destFullPath;
                        std::thread([copyDestFullPath]() {
                            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                            FileOrFolderDelete(copyDestFullPath, true);
                        }).detach();
                    }
                    else {
                        FileOrFolderDelete(destFullPath, true);
                    }

                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                    if (qr.action == actionDELR) updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPSUCCESS);
                    linesProcessed.insert(updatedLine);
                    if (qr.action == actionDELR) linesProcessed.insert(updatedMatchedLine);

                    // 时序控制：检测 DEL 记录是否有关联的重命名记录
                    if (qr.action == actionDEL) {
                        std::set<std::wstring> renNewCriteria = { actionRENNEW };
                        if (!FindQueueRowFromQueueSetByFullPathAAction(qs.otherSet, qr.objFullPath, renNewCriteria).empty()) {
                            deleteHasLinkedRename = true;
                        }
                    }

                    // WriteFileSyncLog(L"文件同步成功", g_emptyWString, destFullPath, 1, 1, 2);
                    continue;
                }
                else {
                    // 这里是针对DEL和DELR外的记录，万一有则归入UNKNOWN进失败日志
                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPUNKNOWN);
                    linesFailed.insert(updatedLine);
                    WriteFileSyncLog(L"folderDeleteSet里存在未识别场景数据，归入失败待分析", qr.objFullPath, g_emptyWString, 1, 2, 4); // 新增，失败，错误。业务定义的异常
                    continue;
                }

            }


            // 最后是剩余部分，文件和文件夹混一起，剩余的就是修改、重命名两类，
            // 前面集合循环时把和它们的关联记录都删除光了，剩余部分应该是单一记录为主
            // 这里DeleteQueueRowFromQueueSetByEventAAction涉及到删除在同一次循环中的同一容器记录，要保证循环和删除容器要相同
            auto lastSet = GetSortedQueueListFromQueueSet(qs, 1);
            std::set<QueueRow> parsedSet(lastSet.begin(), lastSet.end());

            for (const auto& qr : parsedSet) {

                // 预处理，由于single、current模式导致要丢弃的文件，在多目标情况下如果仍无匹配，则跳过
                int countTargets = countValidTargets(qr.objFullPath, qr.objType);
                if (countTargets == 0) {
                    linesDiscarded.insert(qr.originalLine);
                    continue;
                }

                if (qr.action == actionMOD) {
                    // 修改场景不涉及匹配另一条记录，且只有文件
                    destFullPath = GetAllPairedFullPathsForSync(qr.objFullPath, qr.objType);

                    validateDest = DirectoryExistsWithCredential2(destFullPath, 2);
                    // 操作前验证路径是否可访问，0继续，-3入重试，-1是不存在要看场景来处理，其它入失败
                    if (validateDest == -3) {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPRETRY);
                        updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPRETRY);
                        linesRetry.insert(updatedLine);
                        linesRetry.insert(updatedMatchedLine);
                        continue;
                    }
                    else if (validateDest == 0) {
                        // 成功则什么都不做
                    }
                    else if (validateDest == -5 && qr.objType == typeFILE) {
                        // 文件时如果目录不存在，先创建目录链
                        for (const auto& singleDest : destFullPath) {
                            try {
                                std::filesystem::create_directories(std::filesystem::path(GetParentPathNoBackSlash(singleDest)));
                                WriteFileSyncLog(L"目标目录不存在，已自动创建：" + GetParentPathNoBackSlash(singleDest), g_emptyWString, g_emptyWString, 1, 1, 2);
                            }
                            catch (const std::exception& e) {
                                WriteFileSyncLog(L"自动创建目录失败：" + ToWide(e.what()), GetParentPathNoBackSlash(singleDest), g_emptyWString, 1, 2, 4);
                            }
                        }

                    }
                    else {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                        updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                        linesFailed.insert(updatedLine);
                        linesFailed.insert(updatedMatchedLine);
                        continue;
                    }

                    for (const auto& singleDest : destFullPath) {
                        // 如果目标文件存在、文件满足移除锁场景则移除锁，如和移除锁冲突则进失败队列，在os异常之前拦截掉；
                        if (std::filesystem::exists(singleDest)) {
                            srcFile = GetFileObjectAttr(qr.objFullPath);
                            destFile = GetFileObjectAttr(singleDest);
                            if (destFile.isReadOnly && (fileCopyOption == 2 || fileCopyOption == 1) && CompareFileTime(&srcFile.lastWriteTime, &destFile.lastWriteTime) > 0) {
                                if (fileCopyRemoveLock) {
                                    FileRemoveReadOnly(singleDest);
                                }
                                // 目标文件只读、源文件比目标文件新（意思是想要覆盖）、系统开关1或2都是要更新情况下，进入失败分支
                                else {
                                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                                    linesFailed.insert(updatedLine);
                                    WriteFileSyncLog(L"目标文件被锁定，fileCopyRemoveLock禁用，源文件较新，OS异常前拦截", g_emptyWString, singleDest, 1, 2, 4); // 新增，失败，错误。业务定义的异常
                                    continue;
                                }
                            }
                        }
                    }

                    FileOrFolderCopyWIterate(qr.objFullPath, destFullPath, true, recursive, fileCopyOption);
                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                    linesProcessed.insert(updatedLine);
                    continue;
                }

                else {
                    // 除了重命名场景全部放在一起，参数涉及旧名称对象在目标主机上的绝对路径、新名称两个
                    std::wstring newName;
                    std::wstring srcFilePathRenamed;

                    // destFullPath代表目标上存在的改名前旧名称的绝对路径
                    // 文件、文件夹都适用
                    if (qr.action == actionRENOLD || qr.action == actionRENOLDR) {
                        criteria.insert(actionRENNEW);
                        criteria.insert(actionRENNEWR);
                        matchedRow = DeleteQueueRowFromQueueSetByEventAAction(parsedSet, qr.eventId, criteria);
                        criteria.clear();
                        // 改名的旧名称记录得到目标主机下现有对象的绝对路径
                        destFullPath = GetAllPairedFullPathsForSync(qr.objFullPath, qr.objType);
                        // 不用GetAllPairedFullPathsForSync获取，刚好queue记录里有存
                        newName = matchedRow.objName;
                        srcFilePathRenamed = matchedRow.objFullPath;
                    }
                    else if (qr.action == actionRENNEW || qr.action == actionRENNEWR) {
                        criteria.insert(actionRENOLD);
                        criteria.insert(actionRENOLDR);
                        matchedRow = DeleteQueueRowFromQueueSetByEventAAction(parsedSet, qr.eventId, criteria);
                        criteria.clear();
                        // 改名的旧路径在matchedRow上
                        destFullPath = GetAllPairedFullPathsForSync(matchedRow.objFullPath, matchedRow.objType);
                        // 不用GetAllPairedFullPathsForSync获取，刚好queue记录里有存
                        newName = qr.objName;
                        srcFilePathRenamed = qr.objFullPath;
                    }

                    validateDest = DirectoryExistsWithCredential2(destFullPath, 2);
                    // 操作前验证路径是否可访问，0继续，-3入重试，-1是不存在要看场景来处理，其它入失败
                    if (validateDest == -3) {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPRETRY);
                        updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPRETRY);
                        linesRetry.insert(updatedLine);
                        linesRetry.insert(updatedMatchedLine);
                        continue;
                    }
                    else if (validateDest == 0) {
                        // 成功则什么都不做
                    }
                    else {
                        updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                        updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                        linesFailed.insert(updatedLine);
                        linesFailed.insert(updatedMatchedLine);
                        continue;
                    }

                    for (const auto& singleDest : destFullPath) {
                        // 如果目标文件存在、文件满足移除锁场景则移除锁，如和移除锁冲突则进失败队列，在os异常之前拦截掉；
                        // 文件夹和这段代码无关
                        if (qr.objType == typeFILE && std::filesystem::exists(singleDest)) {
                            srcFile = GetFileObjectAttr(srcFilePathRenamed);
                            destFile = GetFileObjectAttr(singleDest);
                            if (destFile.isReadOnly && (fileCopyOption == 2 || fileCopyOption == 1) && CompareFileTime(&srcFile.lastWriteTime, &destFile.lastWriteTime) > 0) {
                                if (fileCopyRemoveLock) {
                                    FileRemoveReadOnly(singleDest);
                                }
                                // 目标文件只读、源文件比目标文件新（意思是想要覆盖）、系统开关1或2都是要更新情况下，进入失败分支
                                else {
                                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPFAILED);
                                    updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPFAILED);
                                    linesFailed.insert(updatedLine);
                                    linesFailed.insert(updatedMatchedLine);
                                    WriteFileSyncLog(L"目标文件被锁定，fileCopyRemoveLock禁用，源文件较新，OS异常前拦截", g_emptyWString, singleDest, 1, 2, 4); // 新增，失败，错误。业务定义的异常
                                    continue;
                                }
                            }
                        }
                    }



                    // 先改名，让目标的绝对路径按新名称先发生变化，其它属性都没变
                    // 并适用于文件夹
                    if (addHasLinkedRename || deleteHasLinkedRename) {
                        auto copyDestFullPath = destFullPath;
                        auto copyNewName = newName;
                        std::thread([copyDestFullPath, copyNewName]() {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
                            FileOrFolderRename(copyDestFullPath, copyNewName);
                        }).detach();
                    }
                    else {
                        FileOrFolderRename(destFullPath, newName);
                    }

                    // 如果是文件，那么考虑改名前后文件有修改情况（RENNAME OLD R或NEW R）
                    // 在改名后继续发起一次尝试复制，采用的还是全局的文件复制option。至少改名可以保障，文件是否覆盖看fileCopyOption
                    // 两次命令中间不需要设延时，让windows在os层面自行控制，命令不会丢就行
                    if (qr.objType == typeFILE) {
                        destFullPath = GetAllPairedFullPathsForSync(srcFilePathRenamed, qr.objType);
                        if (addHasLinkedRename || deleteHasLinkedRename) {
                            auto copyDestFullPath = destFullPath;
                            auto copySrcFilePathRenamed = srcFilePathRenamed;
                            auto copyFileCopyOption = fileCopyOption;
                            std::thread([copyDestFullPath, copySrcFilePathRenamed, recursive, copyFileCopyOption]() {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1100));
                                FileOrFolderCopyWIterate(copySrcFilePathRenamed, copyDestFullPath, true, recursive, copyFileCopyOption);
                            }).detach();
                        }
                        else {
                            FileOrFolderCopyWIterate(srcFilePathRenamed, destFullPath, true, recursive, fileCopyOption);
                        }
                    }
                    // 如果是文件夹，改名后同样发起一次拷贝兜底，确保在 ADD+RENAME 同批次场景下
                    // （源端旧名称已不存在导致 folderAddSet 的 ADD 拷贝失败时）目标端能用新名称正确创建
                    else if (qr.objType == typeFOLDER) {
                        auto newFolderDest = GetAllPairedFullPathsForSync(srcFilePathRenamed, qr.objType);
                        if (addHasLinkedRename || deleteHasLinkedRename) {
                            auto copyDest = newFolderDest;
                            auto copySrc = srcFilePathRenamed;
                            auto autoFileCopyOption = fileCopyOption;
                            std::thread([copyDest, copySrc, recursive, autoFileCopyOption]() {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1100));
                                FileOrFolderCopy(copySrc, copyDest, true, recursive, autoFileCopyOption);
                            }).detach();
                        }
                        else {
                            FileOrFolderCopy(srcFilePathRenamed, newFolderDest, true, recursive, fileCopyOption);
                        }
                    }

                    // 不用判断，一定是成对
                    updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPSUCCESS);
                    updatedMatchedLine = UpdateLineElementByIndex(matchedRow.originalLine, 7, aStatusPSUCCESS);
                    linesProcessed.insert(updatedLine);
                    linesProcessed.insert(updatedMatchedLine);
                    // WriteFileSyncLog(L"文件同步成功", g_emptyWString, destFullPath, 1, 1, 2);
                    continue;
                }

                // 这里是针对MOD和RENAME外的记录，万一有则归入UNKNOWN进失败日志
                updatedLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusPUNKNOWN);
                linesFailed.insert(updatedLine);
                WriteFileSyncLog(L"otherSet里存在未识别场景数据，归入失败待分析", qr.objFullPath, g_emptyWString, 1, 2, 4); // 新增，失败，错误。业务定义的异常
            }

            // 整体最后处理一遍，把queue的objName字段更新为文件大小，取不到信息或和文件无关的对象留空
            // 要复用objName只能这里处理，前面判断要用到
            for (const auto& line : linesProcessed) {
                QueueRow qr = ParseQueueLine(line);
                if (qr.empty() || qr.objFullPath.empty()) continue;
				int countTargets = countValidTargets(qr.objFullPath, qr.objType);
				// 插入第三个字段为文件大小，更新完后总字段数量8
                if (qr.objType == typeFOLDER) {
                    updatedLine = InsertLineElementByIndex(line, 3, g_emptyWString);
                }
                else {
                    srcFile = GetFileObjectAttr(qr.objFullPath);
                    if (srcFile.exists()) {
                        updatedLine = InsertLineElementByIndex(line, 3, srcFile.fileHumanSize);
                    }
                    else {
                        updatedLine = InsertLineElementByIndex(line, 3, g_emptyWString);
                    }
                }
                // updatedLine = updatedLine + L"    \"[DEST] " + std::to_wstring(countTargets) + L"\"";
				updatedLine = InsertLineElementByIndex(updatedLine, 9, L"[" + std::to_wstring(countTargets) + L"]"); // 第9个位置插入目标数量信息，更新完后总字段数量9
				updatedLine = MoveLineElementByIndex(updatedLine, 5, 9); // 把原完整绝对路径挪到第9个位置（即最后），挪完总字段数量还是9
				linesProcessedSizeAdded.insert(updatedLine);
            }

			// fileSQ文件首次写入时添加字段头
            // 需要和上面linesProcessedSizeAdded集合处理完之后的格式保持一致
            // std:wstring fileSQHeader = L"\"ACTION\"    \"TYPE\"    \"SIZE\"    \"FILE/FOLDER NAME\"    \"DATE/TIMESTAMP\"    \"EVENT ID\"    \"PROCESS STATUS\"    \"NUMBER OF TARGETS\"    \"PATH\"\r\n";
            std::wstring fileSQHeader = L"\"操作类型\"    \"目录/文件\"    \"文件大小\"    \"目录/文件名称\"    \"操作时间戳\"    \"事件编号\"    \"队列状态\"    \"同步目标数量\"    \"绝对路径\"\r\n";
            std::wifstream ifs(fileSQ); wchar_t c;
            if (!(ifs >> c)) WriteUTF8LinesToFile(fileSQ, nullptr, std::set<std::wstring>{fileSQHeader}, 2);
            // 追加模式，只有非空集合才写文件
            if (!linesProcessedSizeAdded.empty() && disableFileQueueLog == 0) WriteUTF8LinesToFile(fileSQ, nullptr, linesProcessedSizeAdded, 2);
            if (!linesRetry.empty()) WriteUTF8LinesToFile(fileRQ, nullptr, linesRetry, 2);
            if (!linesFailed.empty()) WriteUTF8LinesToFile(fileFQ, nullptr, linesFailed, 2);
            if (!linesDiscarded.empty() && disableModeCurrentAndFileLog == 0) WriteUTF8LinesToFile(fileDQ, nullptr, linesDiscarded, 2);

            // 处理完毕后删除临时文件
            fs::path tFilePath = fs::absolute(tempQ);
            if (fs::exists(tFilePath)) {
                std::filesystem::remove(tFilePath);
            }

        } // try 结束

        catch (const std::exception& e) {
            WriteUTF8LinesToFile(fileFQ, nullptr, linesFailed, 2);
            WriteDebugExceptionHex("std::exception", e.what(), "");
        } // try catch结束，这部分是集合的处理
    }
}





// 重构版本，统一处理set容器后再统一分拣写入文件。一定要同步绑定在ProcessQueue里执行，否则中间过程产生的新原始记录无法再次触发合并
// callType：1 - 由原始windows触发文件变更+防抖+office类防抖调用，2 - 由Merge自身特定场景重试发起时调用。异步延时不同
void MergeQueue(const int& callType) {
    if (callType != 1 && callType != 2) {
        std::wstring wmsg = L"MergeQueue调用参数非法，callType必须为1或2";
        WriteMergeLog(4, wmsg, g_emptyWString, 3);
        return;
    }

    g_mergeRunning = true;

    int waitMilli = (callType == 1) ? mergeDelayMilli : mergeRetryMilli;
    // 采用新的调度器，这里不睡眠了，交给TryTriggerQueue管理
    // std::this_thread::sleep_for(std::chrono::milliseconds(waitMilli));
    // 涉及待处理文件路径
    std::wstring fullPath = qPath + L"/" + listenQ;
    std::wstring mergeSQ = qPath + L"/" + mergeSuccessQ;
    std::wstring mergeRQ = qPath + L"/" + mergeRawQ;
    std::wstring mergeFQ = qPath + L"/" + mergeFailedQ;
    std::wstring mergeBQ = qPath + L"/" + mergeBadQ;
    std::wstring mergeEQ = qPath + L"/" + mergeExcludedQ;



    // 准备set容器
    std::set<std::wstring> linesPassed;  // Merge初始处理成功记录，还未合并
    std::set<std::wstring> linesPassMerged; // 多个同名的队列记录的成功合并进入本集合
    std::set<std::wstring> linesBad; // 格式不通过坏记录
    std::set<std::wstring> linesFailed; // 重试后最终失败记录，和原始记录相比没有内容变化
    std::set<std::wstring> linesCorrected; // 被修正之后新记录进入成功，这里备份原始记录的值
    std::set<std::wstring> linesExcluded; // 用户过滤记录，算是一种备份供查看
    std::set<std::wstring> linesFinalMerged; // 写入成功队列文件的set
    // 记录因为监听目录因为被配置为current时而过滤掉的所有子目录树下的变更，只存原始行内容不修正，方法里暂时不对该集合后续处理，直接过滤


    // 作用域内锁定并检查文件状态
    // 读原文件得到lines，仍需要保留在原始队列里的只有重试，留在linesRemaining写回listenQ
    std::vector<std::wstring> lines;
    std::set<std::wstring> linesRemaining;
    {
        try {
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);

                if (!fs::exists(qPath)) fs::create_directories(qPath);
                if (!fs::exists(fullPath)) {
                    std::ofstream outfile(fullPath); outfile.flush(); outfile.close();
                }
                lines = FileToWStringLinesXY(fullPath);

                for (auto it = lines.begin(); it != lines.end(); ++it) {

                    // 基本格式先处理下
                    std::wstring line = FormattedLine(*it);

                    // 去除无效行、注释行
                    if (line.empty() || line[0] == L'#' || (line.size() >= 2 && line[0] == L'/' && line[1] == L'/')) continue;

                    // ===== 预校验，处理坏记录 =====
                    auto vr = ValidateQueueLine(line);
                    if (!vr.valid) {
                        WriteMergeLog(4, L"队列行校验失败：" + vr.error + L"，记录被移入：" + mergeBQ, vr.eventId, 3);
                        linesBad.insert(line);
                        continue;
                    }

                    QueueRow row = ParseQueueLine(line);
                    if (row.action.empty()) continue;
                    // 当前循环中的原始待merge程序处理的queue记录
                    std::wstring action = row.action;
                    std::wstring objType = row.objType;
                    std::wstring objName = row.objName;
                    std::wstring objFullPath = row.objFullPath;
                    std::wstring timestamp = row.timestamp;
                    std::wstring eventId = row.eventId;
                    std::wstring status = row.status;
                    objFullPath = FormatPath(objFullPath);


                    // ===== 先把满足条件的主动过滤记录先挪队列文件 =====
                    if (needExcludeByExcludeConfig(objFullPath)) {
                        std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMEXCLUDED);
                        linesExcluded.insert(newWline);
                        std::wstring wmsg = L"队列匹配排除规则，移入文件：" + mergeEQ;
                        WriteMergeLog(1, wmsg, eventId, 2);
                        continue;
                    }


                    // ===== 场景 1 =====
                    if (action == actionMOD && objType == typeFOLDER) {
                        int retryTimes = GetQueueRetryTimes(status);
                        if (retryTimes > mergeRetryTimes) {
                            // 超过最大重试次数，记录挪入最后失败queue文件，从当前queue删除
                            std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMREMOVED);
                            linesCorrected.insert(newWline);
                            std::wstring wmsg = L"类型为MODIFIED的FOLDER记录超过最大重试次数，跳过处理";
                            WriteMergeLog(3, wmsg, eventId, 3);
                        }
                        else {
                            // 这里还要预留一段代码，设置重试次数。目前没设置是因为还没有看到有移记录失败处理不下去的例子，否则就得设置status让下一次留在原始队列等待重处理
                            std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMREMOVED);
                            // 使用日志级别来确定是否要输出，因为有时候大目录复制可能数据量太多，数据本身没意义
                            if (g_logLevel <= 2) linesCorrected.insert(newWline);
                            std::wstring wmsg = L"直接删除类型为MODIFIED的FOLDER记录成功：" + objName + L"，原始动作类型：" + action;
                            WriteMergeLog(1, wmsg, eventId, 2);
                        }
                        continue;
                    }


                    // ===== 场景 2：先循环到RENAME OLD记录
                    if (action == actionRENOLD) {
                        // 相同event id但动作为NEW的记录
                        std::vector<std::wstring> eventLines = FindQueueLines(lines, eventId, actionRENNEW, L"*", L"*", L"*", L"*", L"*", L"ASC");
                        // 匹配到NEW记录
                        if (eventLines.size() == 1) {
                            // 只有一条NEW记录，理想情况
                            std::wstring matchedLine = eventLines.at(0);
                            // 获取NEW记录的对象类型并新增到队列文件
                            auto row = ParseQueueLine(matchedLine);

                            std::wstring matchedObjType = row.objType;
                            std::wstring matchedFullPath = row.objFullPath;
                            std::wstring matchedEventId = row.eventId;

                            // 如果OLD和NEW的类型不一致，则需要更新OLD记录的类型
                            if (matchedObjType != objType) {
                                // 送备份
                                std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMCORRECTED);
                                linesCorrected.insert(newWline);
                                // 处理成功后送成功队列
                                newWline = UpdateLineElementByIndex(newWline, 7, aStatusMSUCCESS);
                                newWline = UpdateLineElementByIndex(newWline, 2, matchedObjType);
                                linesPassed.insert(newWline); // 更新旧记录状态、对象类型，加入成功清单
                                std::wstring wmsg = L"处理成功，OLD记录的类型刷新为NEW记录类型";
                                WriteMergeLog(1, wmsg, eventId, 2);
                                // 如果重命名old被更正为文件夹，需要在这里把目录清单从内存删除
                                if (matchedObjType == typeFOLDER && objType == typeFILE) DeleteSubFolderFromMem(objFullPath);
                            }
                            else {
                                // 如果一致代表肯定正确的类型是文件，两条记录都是文件，在同一循环里都匹配到了，场景2和3都会匹配一次
                                // 因此这个循环里只能挪当前记录，否则场景2、3会各走一次循环导致一对记录处理两次
                                std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMSUCCESS);
                                linesPassed.insert(newWline);
                                std::wstring wmsg = L"重命名记录均为文件，本条OLD，移入成功队列";
                                WriteMergeLog(1, wmsg, eventId, 2);
                            }
                        }
                        else if (eventLines.size() == 0) {
                            // 表示循环到OLD时还查不到NEW记录，可能是NEW还没写入队列，跳过等待下一轮，处理，试满5次后移到失败queue
                            int retryTimes = GetQueueRetryTimes(status);
                            if ((ContainStringOrEqual(status, aStatusMRETRY) || status.empty() || status == aStatusINIT) && retryTimes < mergeRetryTimes) {
                                // 在重试次数之内每次都触发一个异步来重试
                                std::wstring newRretryTimes = aStatusMRETRY + L", " + std::to_wstring(retryTimes + 1); // GetQueueRetryTimes需要逗号判断分割，这里要加逗号
                                std::wstring newWline = UpdateLineElementByIndex(line, 7, newRretryTimes);
                                linesRemaining.insert(newWline); // 只有重试需要添加回给listen queue便于能重试
                                std::wstring wmsg = L"RENAME_OLD未找到对应的NEW记录，重试一次（" + std::to_wstring(retryTimes) + L"）";
                                WriteMergeLog(2, wmsg, eventId, 2);
                                // 重试触发传递队列长度
                                // TryTriggerMergeQueue(2, 0);
                                XYEventScheduler(MergeQueueCtx, 1, EventProcessType::MergeQueue);
                            }
                            else {
                                // 重试次数超过最大，进入失败队列
                                // 状态字段应是原先的值，不更新状态值

                                linesFailed.insert(line);
                                std::wstring wmsg = L"RENAME OLD记录找不到NEW，重试满" + std::to_wstring(mergeRetryTimes) + L"次移入失败队列";
                                WriteMergeLog(3, wmsg, eventId, 3);
                            }
                        }
                        else {
                            // 异常情况，理论上不应该出现多条NEW记录，送入失败队列，出现了就要优化这里代码。不保留在原listen队列
                            std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMUNKNOWN);
                            linesFailed.insert(newWline);
                            std::wstring wmsg = L"异常情况：找到多条对应的NEW记录，跳过该OLD处理（出现该记录需要优化代码）";
                            WriteMergeLog(3, wmsg, eventId, 3);
                        }
                        continue;
                    }




                    // ===== 场景 ：EXCEL特定情况非常规记录合并
                    // 判断特殊的excel通过另存为方式下落下来的一条RENNEW一条DELETE特殊情况，原始防抖和文件名过滤实在调不出来，这里特殊处理，提前于场景2和场景3
                    // 原始的类型组合过于奇葩，此前从未碰到
                    if (action == actionRENNEW && objType == typeFILE && isExcelFile(objFullPath)) {
                        std::vector<std::wstring> matchedLines = FindQueueLines(lines, L"*", actionDEL, L"*", L"*", L"*", objFullPath, L"*", L"ASC");
                        if (matchedLines.size() > 0) {
                            QueueRow matchedQueue = ParseQueueLine(matchedLines.at(0));
                            if (almostSameTime(timestamp, matchedQueue.timestamp, 2)) {
                                // 相差2秒之内
                                std::wstring newline = UpdateLineElementByIndex(matchedQueue.originalLine, 7, aStatusMSUCCESS);
                                newline = UpdateLineElementByIndex(newline, 1, actionADD);
                                linesPassed.insert(newline); // 更新为ADD类型，加入成功清单
                                newline = UpdateLineElementByIndex(line, 7, aStatusMCORRECTED);
                                linesCorrected.insert(newline);
                                std::wstring wmsg = L"Excel特殊场景判断合并成功";
                                WriteMergeLog(1, wmsg, eventId, 2);
                                continue;
                            }
                        }
                    }
                    // 先循环到特殊Excel的删除这条，上面的部分会处理linesPassed，这里只添加linesCorrected
                    if (action == actionDEL && objType == typeFILE && isExcelFile(objFullPath)) {
                        std::vector<std::wstring> matchedLines = FindQueueLines(lines, L"*", actionRENNEW, L"*", L"*", L"*", objFullPath, L"*", L"ASC");
                        if (matchedLines.size() > 0) {
                            QueueRow matchedQueue = ParseQueueLine(matchedLines.at(0));
                            if (almostSameTime(timestamp, matchedQueue.timestamp, 2)) {
                                // 相差2秒之内
                                std::wstring newline = UpdateLineElementByIndex(line, 7, aStatusMCORRECTED);
                                linesCorrected.insert(newline);
                                continue;
                            }
                        }
                    }


                    // ===== 场景 3：先循环到RENAME NEW记录
                    if (action == actionRENNEW) {

                        // 相同event id但动作为OLD的记录
                        std::vector<std::wstring> eventLines = FindQueueLines(lines, eventId, actionRENOLD, L"*", L"*", L"*", L"*", L"*", L"ASC");

                        // 匹配到OLD记录
                        if (eventLines.size() == 1) {
                            // 只有一条OLD记录，理想情况
                            std::wstring matchedLine = eventLines.at(0);
                            // 获取OLD记录的对象类型并新增到队列文件
                            auto row = ParseQueueLine(matchedLine);
                            std::wstring matchedObjType = row.objType;
                            std::wstring matchedFullPath = row.objFullPath;
                            std::wstring matchedEventId = row.eventId;
                            // 如果OLD和NEW的类型不一致，则需要更新OLD记录的类型
                            if (matchedObjType != objType) {
                                // 送备份，除了状态更新外其它保持原样，查日志可以知道原数据
                                std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMCORRECTED);
                                linesCorrected.insert(newWline);

                                // 处理成功后送成功队列，matched是RENAME OLD
                                newWline = UpdateLineElementByIndex(newWline, 7, aStatusMSUCCESS);
                                linesPassed.insert(newWline); // 更新新记录状态，加入成功清单

                                std::wstring wmsg = L"处理成功，NEW记录同时送备份和成功队列";
                                WriteMergeLog(1, wmsg, eventId, 2);

                                // 只有当前NEW是FOLDER，匹配到的OLD是FILE时，因为OLD这条实际是目录路径被更正了，代表着目录的删除，才需要把OLD记录从目录列表中删除
                                // 如果反过来匹配到OLD是FOLDER则OLD会被纠正为FILE，但不需要从目录清单删除
                                if (matchedObjType == typeFILE && objType == typeFOLDER) DeleteSubFolderFromMem(matchedFullPath);
                            }
                            else {
                                // 如果一致代表肯定正确的类型是文件，两条记录都是文件，在同一循环里都匹配到了，场景2和3都会匹配一次
                                // 因此这个循环里只能挪当前记录，否则场景2、3会各走一次循环导致一对记录处理两次
                                std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMSUCCESS);
                                linesPassed.insert(newWline);
                                std::wstring wmsg = L"重命名记录均为文件，本条NEW，移入成功队列";
                                WriteMergeLog(1, wmsg, eventId, 2);
                            }
                        }
                        else if (eventLines.size() == 0) {
                            // 表示循环到NEW时还查不到OLD记录，可能是OLD还没写入队列，跳过等待下一轮处理

                            // 在重试次数之内每次都触发一个异步来重试
                            int retryTimes = GetQueueRetryTimes(status);
                            if ((ContainStringOrEqual(status, aStatusMRETRY) || status.empty() || status == aStatusINIT) && retryTimes < mergeRetryTimes) {
                                std::wstring newRretryTimes = aStatusMRETRY + L", " + std::to_wstring(retryTimes + 1); // GetQueueRetryTimes需要逗号判断分割，这里要加逗号
                                std::wstring newWline = UpdateLineElementByIndex(line, 7, newRretryTimes);
                                linesRemaining.insert(newWline); // 只有重试需要添加回给listen queue便于能重试
                                std::wstring wmsg = L"RENAME_NEW未找到对应的OLD记录，重试一次（" + std::to_wstring(retryTimes) + L"）";
                                WriteMergeLog(2, wmsg, eventId, 2);
                                // 重试触发传递队列长度
                                // TryTriggerMergeQueue(2, 0);
                                XYEventScheduler(MergeQueueCtx, 1, EventProcessType::MergeQueue);
                            }
                            else {
                                // 重试次数超过最大，进入失败队列
                                // 状态字段应是原先的值，不更新状态值
                                linesFailed.insert(line);

                                std::wstring wmsg = L"RENAME NEW记录找不到OLD，重试满" + std::to_wstring(mergeRetryTimes) + L"次移入失败队列";
                                WriteMergeLog(3, wmsg, eventId, 3);

                            }
                        }
                        else {
                            // 异常情况，理论上不应该出现多条NEW记录，送入失败队列，出现了就要优化这里代码。不保留在原listen队列
                            std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMUNKNOWN);
                            linesFailed.insert(newWline);
                            std::wstring wmsg = L"异常情况：找到多条对应的OLD记录，跳过该NEW处理（出现该记录需要优化代码）";
                            WriteMergeLog(3, wmsg, eventId, 3);
                        }
                        continue;
                    }

                    // 场景判断的剩余部分，前面没处理完的lines体现在这里
                    // 如果识别出更多需要特殊处理的场景，添加在这个区块上面，并加continue退出当前这条line循环
                    std::wstring newWline = UpdateLineElementByIndex(line, 7, aStatusMSUCCESS);
                    linesPassed.insert(newWline);
                    std::wstring wmsg = L"非特殊处理情况，记录移入成功队列";
                    WriteMergeLog(1, wmsg, eventId, 2);

                } // 第一个初始处理的for结束



                // 多记录复杂场景判断合并
                // 初始的成功清单需要对多条业务记录情况进一步合并，其它集合则直接写入文件
                // linesPassed只用于遍历，自身此时不能写操作，输出使用linesPassMerged
                std::wstring linePassed;
                // std::wstring currentObjType;
                std::wstring currentObjFullPath;
                std::wstring currentEventId;
                std::wstring currentObjName;
                std::wstring currentAction;
                std::wstring currentObjType;

                std::vector<std::wstring> matchedLinesInclSelf;
                std::wstring matchedLine;
                std::wstring matchedAction;
                std::wstring matchedObjType;
                std::wstring matchedEventId;
                std::wstring matchedObjName;
                std::wstring matchedObjFullPatch;
                std::wstring matchedTimestamp;
                std::wstring matchedObjStatus;

                std::wstring cursor; // 临时wstring变量，随时在循环任何地方赋值后当场使用

                size_t countMatchedLines = 0;
                std::wstring newLine;

                // 处理全量预处理成功的记录
                std::unordered_map<std::wstring, std::vector<QueueRow>> groupsByPath;
                for (const auto& lp : linesPassed) {
                    if (lp.empty()) continue;
                    QueueRow qr = ParseQueueLine(lp);
                    groupsByPath[qr.objFullPath].push_back(qr);
                }

                for (const auto& [path, group] : groupsByPath) {
                    countMatchedLines = group.size();
                    if (countMatchedLines == 1) {
                        linesPassMerged.insert(group[0].originalLine);  // QueueRow有line字段存储原始wstring
                        continue;
                    }

                    size_t countADD = 0, countMOD = 0, countRENOLD = 0, countRENNEW = 0, countDEL = 0, countUNKNOWN = 0;
                    for (const auto& qr : group) {
                        if (qr.action == actionADD) ++countADD;
                        else if (qr.action == actionMOD) ++countMOD;
                        else if (qr.action == actionRENOLD) ++countRENOLD;
                        else if (qr.action == actionRENNEW) ++countRENNEW;
                        else if (qr.action == actionDEL) ++countDEL;
                        else if (qr.action == actionUNKNOWN) ++countUNKNOWN;
                    }

                    // MOD次数为0..n，较难判断，先按大类分成三大类
                    // 如下代码基于一个重要假设：除了修改记录可能有多条之外，其它所有记录同action类型最多只有一条。场景组合情况才可控，代码控制更准确
                    if (countMatchedLines == countMOD) {
                        // 全部都是修改，只取最后一条
                        auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionMOD; });
                        matchedLine = it->originalLine;  // 假设最后一条是按时间或set顺序的最后
                        QueueRow qRowMatched = *it;
                        matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                        // 多条MOD都打merged状态进corrected记录
                        for (const auto& qr : group) {
                            newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                            linesCorrected.insert(newLine);
                        }
                        // 进入队列只有最后一条记录
                        linesPassMerged.insert(matchedLine);

                        WriteMergeLog(1, L"多条修改记录最后一条合并：" + qRowMatched.objName, qRowMatched.eventId, 2);
                        continue;
                    }
                    else if (countMOD == 0) {
                        // 一定没有MOD

                        if (countUNKNOWN > 0) {
                            // 一旦有unknown则所有同名记录推到failed里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                                linesFailed.insert(newLine);
                            }
                            WriteMergeLog(3, L"未知异常，只记录信息供优化代码：" + group[0].objName, group[0].eventId, 3);
                        }
                        else if (countADD == 1 && countRENOLD == 1) {
                            // 增、改名OLD按增输出，信息用改名的记录，event id后面关联要用
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionRENOLD; });
                            matchedLine = it->originalLine;
                            LineElements le = ReadLineElements(matchedLine, g_delimiter);
                            cursor = le.element6;
                            matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                            matchedLine = UpdateLineElementByIndex(matchedLine, 1, actionADDR); // 区别于普通ADD，用于后续处理文件时可以识别判断
                            linesPassMerged.insert(matchedLine);
                            WriteMergeLog(1, L"文件/文件夹增改名成对，合并为一条新增，Event ID使用RENAME OLD：" + group[0].objName, cursor, 2);
                        }
                        else if (countADD == 1 && countRENNEW == 1) {
                            // 增、改名NEW理论上不可能同名，只备份、打异常、从集合删除，但不推到linesPassMerged
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                                linesFailed.insert(newLine);
                            }
                            WriteMergeLog(3, L"异常，捕获到新增+改名NEW一对，不该出现此类数据，代码需查问题：" + group[0].objName, group[0].eventId, 3);
                        }
                        else if (countADD == 1 && countDEL == 1) {
                            // 增、删只作备份，不输出linesPassMerged
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            WriteMergeLog(1, L"文件/文件夹增、删成对，只作备份，Event ID随机日志一条：" + group[0].objName, group[0].eventId, 2);
                        }
                        else if (countRENOLD == 1 && countRENNEW == 1) {
                            // 纯改名，此循环不可能存在
                            // 因为这两个操作的名称不同。纯改名由文件处理进程在一个批次内扫描时判断处理
                            // 什么都不做
                        }
                        else if (countRENOLD == 1 && countDEL == 1) {
                            // 改名OLD和删除理论上不可能同名，只备份、打异常、从集合删除，但不推到linesPassMerged
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                                linesFailed.insert(newLine);
                            }
                            WriteMergeLog(3, L"异常，捕获到改名OLD和删除一对，不该出现此类数据，代码需查问题：" + group[0].objName, group[0].eventId, 3);
                        }
                        else if (countRENNEW == 1 && countDEL == 1) {
                            // 新名称和删一起，以改名NEW数据为基准，按删除输出，类型更新为REMOVED_R
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionRENNEW; });
                            matchedLine = it->originalLine;
                            LineElements le = ReadLineElements(matchedLine, g_delimiter);
                            cursor = le.element6;
                            matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                            matchedLine = UpdateLineElementByIndex(matchedLine, 1, actionDELR); // 区别于普通ADD，用于后续处理文件时可以识别判断
                            linesPassMerged.insert(matchedLine);
                            WriteMergeLog(1, L"文件/文件夹改名、删成对，合并为一条删除，Event ID使用RENAME NEW：" + group[0].objName, cursor, 2);
                        }
                        else {
                            // 预留一个else，有未知场景时通过记录和日志查问题
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                                linesFailed.insert(newLine);
                            }
                            WriteMergeLog(3, L"异常，未知场景，代码需优化：" + group[0].objName, group[0].eventId, 3);
                        } // 没有修改记录时，细分组合场景穷举结束
                    } // 没有MOD场景结束




                    else if (countMatchedLines > countMOD) {
                        // 一定有MOD，也一定有其它，相比于没有MOD的大分支，多了MOD和单一其它action两两组合的情况，已追加
                        if (countUNKNOWN > 0) {
                            // 一旦有unknown则所有同名记录推到failed里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                                linesFailed.insert(newLine);
                            }
                            WriteMergeLog(3, L"未知异常，只记录信息供优化代码：" + group[0].objName, group[0].eventId, 3);
                        }
                        else if (countADD == 1 && countRENOLD == 1) {
                            // 增、改名OLD按增输出，信息用改名的记录，event id后面关联要用
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionRENOLD; });
                            matchedLine = it->originalLine;
                            LineElements le = ReadLineElements(matchedLine, g_delimiter);
                            cursor = le.element6;
                            matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                            matchedLine = UpdateLineElementByIndex(matchedLine, 1, actionADDR); // 区别于普通ADD，用于后续处理文件时可以识别判断
                            linesPassMerged.insert(matchedLine);
                            WriteMergeLog(1, L"文件/文件夹增、修改、改名同时存在，合并为一条新增，Event ID使用RENAME OLD：" + group[0].objName, cursor, 2);
                        }
                        else if (countADD == 1 && countRENNEW == 1) {
                            // 增、改名NEW理论上不可能同名，只备份、打异常、从集合删除，但不推到linesPassMerged
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                                linesFailed.insert(newLine);
                            }
                            WriteMergeLog(3, L"异常，捕获到新增+改名NEW一对，不该出现此类数据，代码需查问题：" + group[0].objName, group[0].eventId, 3);
                        }
                        else if (countADD == 1 && countDEL == 1) {
                            // 增、删只作备份，不输出linesPassMerged
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            WriteMergeLog(1, L"文件/文件夹增、修改、改名同时存在，只作备份，Event ID随机日志一条：" + group[0].objName, group[0].eventId, 2);
                        }
                        else if (countRENOLD == 1 && countRENNEW == 1) {
                            // 纯改名，此循环不可能存在，因为这两个操作的名称不同。纯改名由文件处理进程在一个批次内扫描时判断处理
                            // 什么都不做
                        }
                        else if (countRENOLD == 1 && countDEL == 1) {
                            // 改名OLD和删除理论上不可能同名，只备份、打异常、从集合删除，但不推到linesPassMerged
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                                linesFailed.insert(newLine);
                            }
                            WriteMergeLog(3, L"异常，捕获到改名OLD和删除一对（中间还有修改），不该出现此类数据，代码需查问题：" + group[0].objName, group[0].eventId, 3);
                        }
                        else if (countRENNEW == 1 && countDEL == 1) {
                            // 新名称和删一起，以改名NEW数据为基准，按删除输出，类型更新为REMOVED_R
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionRENNEW; });
                            matchedLine = it->originalLine;
                            LineElements le = ReadLineElements(matchedLine, g_delimiter);
                            cursor = le.element6;
                            matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                            matchedLine = UpdateLineElementByIndex(matchedLine, 1, actionDELR); // 区别于普通ADD，用于后续处理文件时可以识别判断
                            linesPassMerged.insert(matchedLine);
                            WriteMergeLog(1, L"文件/文件夹改名、修改、删同时存在，合并为一条删除，Event ID使用改名的：" + group[0].objName, cursor, 2);
                        }

                        // 接下来的场景是MOD和几类动作两两组合，但分成RENAME OLD组和RENAME NEW两组，目前看到不可能出现的场景这里不穷举，以后从else里的日志和备份记录里追加场景判断
                        else if (countADD == 1 && countRENOLD == 0) {
                            // 新增和修改一起，以修改数据为基准，按新增类型输出
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionMOD; });
                            matchedLine = it->originalLine;
                            LineElements le = ReadLineElements(matchedLine, g_delimiter);
                            cursor = le.element6;
                            matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                            matchedLine = UpdateLineElementByIndex(matchedLine, 1, actionADD);
                            linesPassMerged.insert(matchedLine);
                            WriteMergeLog(1, L"文件/文件夹新增、修改同时存在，合并为一条新增，Event ID使用修改的：" + group[0].objName, cursor, 2);
                        }
                        else if (countADD == 0 && countRENOLD == 1) {
                            // 修改和改名一起，以改名数据为基准，按修改R特殊类型输出（这样能把文件先复制过去再改名、或关联NEW一组动作）
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionRENOLD; });
                            matchedLine = it->originalLine;
                            LineElements le = ReadLineElements(matchedLine, g_delimiter);
                            cursor = le.element6;
                            matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                            matchedLine = UpdateLineElementByIndex(matchedLine, 1, actionRENOLDR); // 区别于普通改名，这是带修改的改名
                            linesPassMerged.insert(matchedLine);
                            WriteMergeLog(1, L"文件/文件夹修改、改名OLD同时存在，合并为一条修改R，Event ID使用改名的：" + group[0].objName, cursor, 2);
                        }
                        else if (countRENNEW == 1 && countDEL == 0) {
                            // 改名新和修改一起，以改名NEW数据为基准，按删除输出，类型更新为REMOVED_R
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionRENNEW; });
                            matchedLine = it->originalLine;
                            LineElements le = ReadLineElements(matchedLine, g_delimiter);
                            cursor = le.element6;
                            matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                            matchedLine = UpdateLineElementByIndex(matchedLine, 1, actionRENNEWR);
                            linesPassMerged.insert(matchedLine);
                            WriteMergeLog(1, L"文件/文件夹修改、改名NEW同时存在，合并为一条修改R，Event ID使用改名的：" + group[0].objName, cursor, 2);
                        }
                        else if (countRENNEW == 0 && countDEL == 1) {
                            // 先修改再删除，合并为删除的一条输出，数据也以删除这条为准
                            // 原样推到备份里
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            // 处理后记录推到linesPassMerged
                            auto it = std::find_if(group.begin(), group.end(), [](const QueueRow& qr) { return qr.action == actionDEL; });
                            matchedLine = it->originalLine;
                            LineElements le = ReadLineElements(matchedLine, g_delimiter);
                            cursor = le.element6;
                            matchedLine = UpdateLineElementByIndex(matchedLine, 7, aStatusMMERGED);
                            linesPassMerged.insert(matchedLine);
                            WriteMergeLog(1, L"文件/文件夹修改、删除同时存在，合并为一条删除，Event ID使用删除的：" + group[0].objName, cursor, 2);
                        }

                        else {
                            // 预留一个else，有未知场景时通过记录和日志查问题
                            for (const auto& qr : group) {
                                newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                                linesFailed.insert(newLine);
                            }
                            WriteMergeLog(3, L"异常，未知场景，代码需优化：" + group[0].objName, group[0].eventId, 3);
                        } // 没有修改记录时，细分组合场景穷举结束


                        continue;
                    }  // 一定有MOD，也一定有其它：场景结束



                    else {
                        //理论上不可能，预留。循环到的记录推到failed里看看
                        for (const auto& qr : group) {
                            newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMFAILED);
                            linesFailed.insert(newLine);
                        }
                        WriteMergeLog(3, L"未知异常，只记录信息供优化代码：" + group[0].objName, group[0].eventId, 3);
                    } // 基于MOD总数的判断：所有else分支结束

                } // groupsByPath所有记录的循环


                // 针对上一段循环的修正程序，对于增删在同一批次内但event id、时间戳都不一样的情况的处理
                // ==================== 不同事件id先删再增按一条增、先增再删对冲掉 ====================
                // 场景：一些程序对同一个文件在极短时间内先删除再新增，在防抖后是两个事件
                std::unordered_map<std::wstring, std::vector<QueueRow>> groupsByFullPath;
                for (const auto& lp : linesPassMerged) {
                    QueueRow qr = ParseQueueLine(lp);
                    groupsByFullPath[qr.objFullPath].push_back(qr);
                }

                std::set<std::wstring> linesAddDelProcessed;  // 本轮处理后的记录集合

                for (const auto& [fullPath, group] : groupsByFullPath) {
                    size_t countMatchedLines = group.size();
                    if (countMatchedLines == 1) {
                        // 只有一条记录 → 直接进入 linesAddDelProcessed
                        linesAddDelProcessed.insert(group[0].originalLine);
                        continue;
                    }

                    size_t countADD = 0, countADDR = 0, countDEL = 0, countDELR = 0;
                    for (const auto& qr : group) {
                        if (qr.action == actionADD) ++countADD;
                        else if (qr.action == actionADDR) ++countADDR;
                        else if (qr.action == actionDEL) ++countDEL;
                        else if (qr.action == actionDELR) ++countDELR;
                    }

                    bool isAddDelPair =
                        (countADD + countADDR == 1) &&
                        (countDEL + countDELR == 1) &&
                        (countMatchedLines == 2);

                    bool isSpecialAddDelDifferentPath = false;
                    if (isAddDelPair) {
                        const auto& p0 = group[0].objFullPath;
                        const auto& p1 = group[1].objFullPath;
                        if (p0 != p1) {
                            isSpecialAddDelDifferentPath = true;
                        }
                    }

                    if (isAddDelPair && !isSpecialAddDelDifferentPath) {
                        // 取出增和删记录
                        QueueRow addRow, delRow;
                        for (const auto& qr : group) {
                            if (qr.action == actionADD || qr.action == actionADDR) addRow = qr;
                            else if (qr.action == actionDEL || qr.action == actionDELR) delRow = qr;
                        }

                        // 按 timestamp 升序 + eventId 升序 判断顺序
                        bool addFirst = false;
                        if (addRow.timestamp != delRow.timestamp) {
                            addFirst = (addRow.timestamp < delRow.timestamp);
                        }
                        else {
                            addFirst = (addRow.eventId < delRow.eventId);
                        }

                        if (addFirst) {
                            // 先增后删 → 对冲：只备份，不进入 linesAddDelProcessed
                            for (const auto& qr : group) {
                                std::wstring newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                                linesCorrected.insert(newLine);
                            }
                            WriteMergeLog(1, L"文件/文件夹增、删成对（按 objFullPath 分组，先增后删），只作对冲备份，路径：" + fullPath, addRow.eventId, 2);
                        }
                        else {
                            // 先删后增 → 保留增记录进入 linesAddDelProcessed，删记录备份
                            linesAddDelProcessed.insert(addRow.originalLine);
                            std::wstring newLine = UpdateLineElementByIndex(delRow.originalLine, 7, aStatusMMERGED);
                            linesCorrected.insert(newLine);
                            WriteMergeLog(1, L"文件/文件夹增、删成对（按 objFullPath 分组，先删后增），保留增记录，路径：" + fullPath, delRow.eventId, 2);
                        }
                    }
                    else {
                        // 不属于增删组合的其它组合 → 所有记录进入 linesAddDelProcessed
                        for (const auto& qr : group) {
                            linesAddDelProcessed.insert(qr.originalLine);
                        }
                    }
                }
                // ================================================



                // 基于第二轮处理结果，处理第三轮合并，这一轮只对冲同一个事件id关联的old组和new组，同时有增删的情况，移入备份队列
                // 前提：第二轮处理时，old组被处理为只剩一条记录，new也同样；如果没有改名事件，则第二轮就已处理只剩一条记录（按名称）
                // std::unordered_map<std::wstring, std::vector<QueueRow>> groupsByEventId;
                // 原处理，linesPassMerged，上面插进了一段循环接管了。改为linesAddDelProcessed
                // 第三轮合并：按 eventId + objName 组合分组处理（对冲增删、特殊拖拽等），一批文件同时拖拽，事件id可能全部共享，所以增加按名称分组
                std::unordered_map<std::pair<std::wstring, std::wstring>, std::vector<QueueRow>, PairHash> groupsByEventIdAndName;
                for (const auto& lp : linesAddDelProcessed) {
                    QueueRow qr = ParseQueueLine(lp);
                    // 使用 eventId + objName 作为复合键分组
                    auto key = std::make_pair(qr.eventId, qr.objName);
                    groupsByEventIdAndName[key].push_back(qr);
                }

                for (const auto& [key, group] : groupsByEventIdAndName) {
                    const std::wstring& eid = key.first;
                    const std::wstring& objName = key.second;

                    countMatchedLines = group.size();
                    if (countMatchedLines == 1) {
                        linesFinalMerged.insert(group[0].originalLine);
                        continue;
                    }

                    size_t countADD = 0, countADDR = 0, countDEL = 0, countDELR = 0;
                    for (const auto& qr : group) {
                        if (qr.action == actionADD) ++countADD;
                        else if (qr.action == actionADDR) ++countADDR;
                        else if (qr.action == actionDEL) ++countDEL;
                        else if (qr.action == actionDELR) ++countDELR;
                    }

                    bool isAddDelPair =
                        (countADD > 0 || countADDR > 0) &&
                        (countDEL > 0 || countDELR > 0);


                    // 一个路径拖到另一个路径的操作，名称相同、绝对路径不同、增删各一条，且增删都不包含R
                    // 当一批文件批量操作时，每个相同名称的一组内比较，不同名称已经在不同分组内。此时这批文件当中可能部分比较集中发生事件的文件共享一个event id
                    bool isSpecialAddDelDifferentPath = false;
                    if (isAddDelPair && group.size() == 2 && countADD == 1 && countDEL == 1) {
                        const auto& p0 = group[0].objFullPath;
                        const auto& p1 = group[1].objFullPath;
                        if (p0 != p1) {
                            isSpecialAddDelDifferentPath = true;
                        }
                    }

                    if (isAddDelPair && !isSpecialAddDelDifferentPath) {
                        // 增、删只作备份，不输出linesPassMerged
                        // 原样推到备份里
                        for (const auto& qr : group) {
                            newLine = UpdateLineElementByIndex(qr.originalLine, 7, aStatusMMERGED);
                            linesCorrected.insert(newLine);
                        }
                        WriteMergeLog(1, L"文件/文件夹增、删成对（最后轮合并处理时），只作对冲备份，本条操作类型：" + group[0].action, group[0].eventId, 2);
                    }
                    else {
                        // 不属于增删组合的其它组合，需要全部进入linesFinalMerged，且从集合里删除，否则下次循环到也乱了
                        // 第二轮处理完之后，相同event id的matchedLine只会有一条，排除当前行自身查询查到的一定是另一条
                        for (const auto& qr : group) {
                            linesFinalMerged.insert(qr.originalLine);
                        }
                    }
                    continue;
                } // 第三轮针对groupsByEventIdAndName处理结束


                // ProcessQueue(1);

                // 调用WriteUTF8LinesToFile写入各文件，这里统一一个锁
                // 只有listen是覆盖当前剩余的部分回去，其它文件都是添加
                // 只有当前处理非空的集合才写文件
                WriteUTF8LinesToFile(fullPath, nullptr, linesRemaining, 1);
                if (!linesFinalMerged.empty()) WriteUTF8LinesToFile(mergeSQ, nullptr, linesFinalMerged, 2);
                if (!linesCorrected.empty() && disableCorrectQueueLog == 0) WriteUTF8LinesToFile(mergeRQ, nullptr, linesCorrected, 2);
                if (!linesFailed.empty()) WriteUTF8LinesToFile(mergeFQ, nullptr, linesFailed, 2);
                if (!linesBad.empty()) WriteUTF8LinesToFile(mergeBQ, nullptr, linesBad, 2);
                if (!linesExcluded.empty() && disableExcludeQueueLog == 0) WriteUTF8LinesToFile(mergeEQ, nullptr, linesExcluded, 2);


                g_mergeRunning = false;

            }
        } // try 结束

        catch (const std::exception& e) {
            WriteDebugExceptionHex("std::exception", e.what(), "");
        } // try catch结束，这部分是集合的处理


    }
}






// 防抖逻辑处理线程
void DebounceWorker() {

    // [NEW] 用于唤醒 DebounceWorker 的事件（由 IOCPWorker 触发）
    HANDLE waitHandles[] = { g_ServiceStopEvent, g_DebounceWakeEvent };

    // [NEW] 是否处于防抖活跃阶段
    bool debounceActive = false;

    // [NEW] 连续空转轮次计数
    int idleRounds = 0;
    const int MAX_IDLE_ROUNDS = 3;

    while (true) {

        // [MODIFIED]
        // 非活跃态：无限等待（真正挂起）
        // 活跃态：保持原 500ms 防抖节奏
        DWORD waitResult = debounceActive
            ? WaitForMultipleObjects(2, waitHandles, FALSE, 500)
            : WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0) {
            // g_ServiceStopEvent
            break;
        }

        if (waitResult == WAIT_OBJECT_0 + 1) {
            // [NEW] IOCPWorker 通知有新事件进入缓存
            debounceActive = true;
            idleRounds = 0;
        }

        if (!debounceActive) {
            continue;
        }

        auto now = std::chrono::system_clock::now();

        // [NEW] 本轮是否处理过任何事件（仅用于 idle 判断）
        bool processedSomething = false;

        // 处理普通缓存
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            for (auto it = g_eventCache.begin(); it != g_eventCache.end(); ) {
                if (now - it->second.lastTime > std::chrono::milliseconds(DebounceWaitMilli)) {
                    std::wstring sFullPath = it->second.path;
                    std::wstring sAction = it->second.action;
                    std::wstring sObjType = it->second.objectType;

                    if (sAction == actionDEL && g_listenAllFolders.count(FormatPath(sFullPath))) {
                        sObjType = typeFOLDER; // 强制修正为文件夹
                        DeleteSubFolderFromMem(sFullPath);
                    }
                    else if (sAction == actionADD || sAction == actionRENNEW) {
                        // 如果是重命名NEW或新增事件，objType是正确的，这里只确定添加下目录到目录列表里
                        if (sObjType == typeFOLDER) AddSingleSubFolderToMem(sFullPath);
                    }
                    else if (sAction == actionMOD || sAction == actionRENOLD) {
                        // 预留，什么都不做，这里的objType不准。在MergeQueue里需要对RENAME OLD如果确定是文件夹的话，对RENAME OLD做一次从列表里删除的动作
                    }

                    PushToQueue(sFullPath, sAction, it->second.eventId, sObjType, g_emptyWString, 1);
                    it = g_eventCache.erase(it);

                    processedSomething = true; // [NEW]
                }
                else it++;
            }
        }

        // 处理重命名对冲过期
        {
            std::lock_guard<std::mutex> lock(g_renameMutex);
            for (auto it = g_renamePending.begin(); it != g_renamePending.end(); ) {
                if (now - it->second.timestamp > std::chrono::milliseconds(DebounceRenameWaitMilli)) {

                    std::wstring sFullPath = it->second.path;
                    std::wstring sAction = it->second.action;
                    std::wstring sObjType = it->second.objType;

                    if (sAction == actionDEL && g_listenAllFolders.count(FormatPath(sFullPath))) {
                        sObjType = typeFOLDER; // 强制修正为文件夹
                        DeleteSubFolderFromMem(sFullPath);
                    }
                    else if (sAction == actionADD || sAction == actionRENNEW) {
                        // 如果是重命名NEW或新增事件，objType是正确的，这里只确定添加下目录到目录列表里
                        if (sObjType == typeFOLDER) AddSingleSubFolderToMem(sFullPath);
                    }
                    else if (sAction == actionMOD || sAction == actionRENOLD) {
                        // 预留，什么都不做，这里的objType不准。在MergeQueue里需要对RENAME OLD如果确定是文件夹的话，对RENAME OLD做一次从列表里删除的动作
                    }

                    PushToQueue(sFullPath, sAction, it->second.eventId, sObjType, g_emptyWString, 1);
                    it = g_renamePending.erase(it);

                    processedSomething = true; // [NEW]
                }
                else it++;
            }
        }

        // [NEW] idle 轮次判断：连续 MAX_IDLE_ROUNDS 轮无处理则挂起
        if (processedSomething) {
            idleRounds = 0;
        }
        else {
            idleRounds++;
            if (idleRounds >= MAX_IDLE_ROUNDS) {
                debounceActive = false;
            }
        }
    }

    // --- 线程退出时的强制清理 ---
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        for (auto& item : g_eventCache) {

            std::wstring sFullPath = item.second.path;
            std::wstring sAction = item.second.action;
            std::wstring sObjType = item.second.objectType;

            if (sAction == actionDEL && g_listenAllFolders.count(FormatPath(sFullPath))) {
                sObjType = typeFOLDER; // 强制修正为文件夹
                DeleteSubFolderFromMem(sFullPath);
            }
            else if (sAction == actionADD || sAction == actionRENNEW) {
                // 如果是重命名NEW或新增事件，objType是正确的，这里只确定添加下目录到目录列表里
                if (sObjType == typeFOLDER) AddSingleSubFolderToMem(sFullPath);
            }
            else if (sAction == actionMOD || sAction == actionRENOLD) {
                // 预留，什么都不做，这里的objType不准。在MergeQueue里需要对RENAME OLD如果确定是文件夹的话，对RENAME OLD做一次从列表里删除的动作
            }
            if (!g_shutdownFastExit.load()) {
                PushToQueue(sFullPath, sAction, item.second.eventId, sObjType, g_emptyWString, 1);
            }
        }
        g_eventCache.clear();
    }
    {
        std::lock_guard<std::mutex> rlock(g_renameMutex);
        for (auto& item : g_renamePending) {

            std::wstring sFullPath = item.second.path;
            std::wstring sAction = item.second.action;
            std::wstring sObjType = item.second.objType;

            if (sAction == actionDEL && g_listenAllFolders.count(FormatPath(sFullPath))) {
                sObjType = typeFOLDER; // 强制修正为文件夹
                DeleteSubFolderFromMem(sFullPath);
            }
            else if (sAction == actionADD || sAction == actionRENNEW) {
                // 如果是重命名NEW或新增事件，objType是正确的，这里只确定添加下目录到目录列表里
                if (sObjType == typeFOLDER) AddSingleSubFolderToMem(sFullPath);
            }
            else if (sAction == actionMOD || sAction == actionRENOLD) {
                // 预留，什么都不做，这里的objType不准。在MergeQueue里需要对RENAME OLD如果确定是文件夹的话，对RENAME OLD做一次从列表里删除的动作
            }

            if (!g_shutdownFastExit.load()) {
                PushToQueue(sFullPath, sAction, item.second.eventId, sObjType, g_emptyWString, 1);
            }
        }
        g_renamePending.clear();
    }
}








// IOCP 工作线程
void IOCPWorker() {
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED pov; while (GetQueuedCompletionStatus(g_hCompPort, &bytes, &key, &pov, INFINITE)) {
        if (key == 0 && pov == nullptr) break;

        DirOverlapped* dio = CONTAINING_RECORD(pov, DirOverlapped, ov);
        DirContext* ctx = dio->ctx;
        PFILE_NOTIFY_INFORMATION pNotify = (PFILE_NOTIFY_INFORMATION)ctx->buffer;

        // 为这一批次（当前 Buffer）生成一个基础 ID
        std::wstring currentBatchId = std::to_wstring(g_GlobalEventId++);

        do {
            // 1. 获取文件名并拼接完整路径
            std::wstring name(pNotify->FileName, pNotify->FileNameLength / 2);
            std::wstring fullPath = (fs::path(ctx->wPath) / name).wstring();

            // 2. 只有在不忽略的情况下才执行业务逻辑
            if (!ShouldIgnore(fullPath)) {

                std::wstring actionStr;
                std::wstring objType = typeFILE;

                // 判定是文件还是文件夹
                DWORD dwAttrib = GetFileAttributesW(fullPath.c_str());
                if (dwAttrib != INVALID_FILE_ATTRIBUTES) {
                    // 如果对象还在，以实时检测为准
                    if (dwAttrib & FILE_ATTRIBUTE_DIRECTORY) {
                        objType = typeFOLDER;
                    }
                }
                else {
                    // 如果对象已消失（REMOVED场景），尝试从现有缓存中追溯历史类型
                    // 1. 尝试从普通防抖缓存找
                    {
                        std::lock_guard<std::mutex> lock(g_cacheMutex);
                        auto it = g_eventCache.find(fullPath);
                        if (it != g_eventCache.end()) {
                            objType = it->second.objectType;
                        }
                        else {
                            // 2. 尝试从重命名缓存找
                            std::lock_guard<std::mutex> rlock(g_renameMutex);
                            auto rit = g_renamePending.find(fullPath);
                            if (rit != g_renamePending.end()) {
                                objType = rit->second.objType;
                            }
                        }
                    }
                }

                // 确定操作类型
                switch (pNotify->Action) {
                case FILE_ACTION_ADDED: actionStr = actionADD; break;
                case FILE_ACTION_REMOVED: actionStr = actionDEL; break;
                case FILE_ACTION_MODIFIED: actionStr = actionMOD; break;
                case FILE_ACTION_RENAMED_OLD_NAME: actionStr = actionRENOLD; break;
                case FILE_ACTION_RENAMED_NEW_NAME: actionStr = actionRENNEW; break;
                default: actionStr = actionUNKNOWN; break;
                }

                // 3. 处理重命名对冲 (RENAME_OLD / RENAME_NEW)
                if (pNotify->Action == FILE_ACTION_RENAMED_OLD_NAME || pNotify->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                    std::lock_guard<std::mutex> rlock(g_renameMutex);
                    auto it = g_renamePending.find(fullPath);
                    if (it != g_renamePending.end()) {
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now() - it->second.timestamp).count();

                        if (duration <= 1000) {
                            // 同一路径 1 秒内对冲成功 -> 合并为一次修改
                            // 这里不增加对objType的判断逻辑，因为修改类型时首先对文件夹没意义（文件夹的修改只有重命名有意义，不存在内容上的修改）
                            // 而文件的重命名本身通过MergeQueue后续就能修正、文件的Modify通过防抖之后能修正为MODIFY，只有下面的else按照缓存里存的
                            PushToQueue(fullPath, actionMOD, currentBatchId, objType, g_emptyWString, 1);
                            g_renamePending.erase(it);
                        }
                        else {

                            std::wstring sFullPath = it->second.path;
                            std::wstring sAction = it->second.action;
                            std::wstring sObjType = it->second.objType;

                            if (sAction == actionDEL && g_listenAllFolders.count(FormatPath(sFullPath))) {
                                sObjType = typeFOLDER; // 此时sObjType不准，强制修正为文件夹，只有当缓存里有这个路径时才判定为文件夹
                                DeleteSubFolderFromMem(sFullPath);
                            }
                            else if (sAction == actionADD || sAction == actionRENNEW) {
                                // 如果是重命名NEW或新增事件，objType是正确的，这里只确定添加下目录到目录列表里
                                if (objType == typeFOLDER) AddSingleSubFolderToMem(sFullPath);
                            }
                            else if (sAction == actionMOD || sAction == actionRENOLD) {
                                // 预留，什么都不做，这里的objType不准。在MergeQueue里需要对RENAME OLD如果确定是文件夹的话，对RENAME OLD做一次从列表里删除的动作
                            }

                            // 时间太久，写出旧的（PushToQueue），存入新的
                            PushToQueue(sFullPath, sAction, it->second.eventId, sObjType, g_emptyWString, 1);
                            g_renamePending[fullPath] = { fullPath, actionStr, objType, currentBatchId, std::chrono::system_clock::now() };
                            // [NEW] 通知 DebounceWorker 有新防抖数据
                            SetEvent(g_DebounceWakeEvent);
                        }
                    }
                    else {
                        // 首次发现，暂存等待
                        g_renamePending[fullPath] = { fullPath, actionStr, objType, currentBatchId, std::chrono::system_clock::now() };
                        // [NEW] 通知 DebounceWorker 有新防抖数据
                        SetEvent(g_DebounceWakeEvent);
                    }
                }
                // 4. 处理普通防抖 (ADDED / REMOVED / MODIFIED)
                else {
                    std::lock_guard<std::mutex> lock(g_cacheMutex);
                    auto now = std::chrono::system_clock::now();
                    std::wstring finalAction = actionStr;

                    // 复制保护：ADDED/REMOVED 状态不被 MODIFIED 覆盖
                    auto it = g_eventCache.find(fullPath);
                    if (it != g_eventCache.end() && it->second.action == actionADD && actionStr == actionMOD) {
                        finalAction = actionADD;
                    }
                    else if (it != g_eventCache.end() && it->second.action == actionDEL && actionStr == actionMOD) {
                        finalAction = actionDEL;
                    }
                    g_eventCache[fullPath] = { fullPath, finalAction, now, currentBatchId, objType };
                    // [NEW] 通知 DebounceWorker 有新防抖数据
                    SetEvent(g_DebounceWakeEvent);
                }
            }

            // 无论是否被 ShouldIgnore 过滤，都必须执行指针跳转
            if (pNotify->NextEntryOffset == 0) break;
            pNotify = (PFILE_NOTIFY_INFORMATION)((LPBYTE)pNotify + pNotify->NextEntryOffset);

        } while (true);

        // 重新发起监听
        try {
            if (!ReadDirectoryChangesW(ctx->hDir, ctx->buffer, BUFFER_SIZE, TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                NULL, &ctx->io.ov, NULL))
            {
                std::wstring wmsg = L"ReadDirectoryChangesW 重新注册失败";
                WriteError(wmsg, ctx->wPath, 4);
            }
        }
        catch (const std::exception& e) {
            std::wstring wmsg = L"调用ReadDirectoryChangesW异常：" + ToWide(e.what());
            WriteError(wmsg, g_emptyWString, 4);
        }
    }

    DWORD lastErr = GetLastError();
    // 屏蔽以下错误码：
    // 0: 正常
    // 735 (ERROR_ABANDONED_WAIT_0): 句柄在等待中被关闭
    // 995 (ERROR_OPERATION_ABORTED): 正在进行的 IO 被取消
    // 87 (ERROR_INVALID_PARAMETER): 句柄已失效导致参数检测失败（停止时的常见现象）
    if (lastErr != 0 && lastErr != 2 && lastErr != ERROR_ABANDONED_WAIT_0 && lastErr != ERROR_OPERATION_ABORTED && lastErr != ERROR_INVALID_PARAMETER) {
        std::wstring wmsg = L"IOCP 工作线程异常终止，错误码：" + std::to_wstring(lastErr);
        WriteError(wmsg, g_emptyWString, 4);
    }

    // 关机极速退出：不做重清理
    if (!g_shutdownFastExit.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_dirMutex);
        for (auto* ctx : g_allDirs) {
            if (ctx->hDir != INVALID_HANDLE_VALUE) CloseHandle(ctx->hDir);
            delete ctx;
        }
        g_allDirs.clear();
    }

}





// --- 服务基础管理 ---
// 这里的执行还是同步的（手动、win重启、win关机）
// windows关机重启这个新API有效，旧的ServiceCtrlHandler删除（不触发）
DWORD WINAPI ServiceCtrlHandlerEx(
    DWORD ctrl,
    DWORD /*eventType*/,
    LPVOID /*eventData*/,
    LPVOID /*context*/
) {
    switch (ctrl) {
    case SERVICE_CONTROL_PRESHUTDOWN:
        WriteLog(L"服务预停止", 5);
        WriteLogToFile();
        WriteErrorToFile();
        deleteRunningCheckFile();
        g_shutdownFastExit.store(true, std::memory_order_relaxed);
        g_running = false; // 标记程序不再运行

        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWaitHint = 3000;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        SetEvent(g_ServiceStopEvent);
        SetEvent(g_DebounceWakeEvent);

        PostQueuedCompletionStatus(g_hCompPort, 0, 0, nullptr);

        return NO_ERROR;


    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        WriteLog(L"服务停止", 5);
        WriteLogToFile();
        WriteErrorToFile();
        deleteRunningCheckFile();

        g_shutdownFastExit.store(true, std::memory_order_relaxed);
        g_running = false; // 标记程序不再运行

        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWaitHint = 4000;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        SetEvent(g_ServiceStopEvent);
        SetEvent(g_DebounceWakeEvent);

        if (g_hCompPort != INVALID_HANDLE_VALUE) {
            PostQueuedCompletionStatus(g_hCompPort, 0, 0, nullptr);
            g_hCompPort = INVALID_HANDLE_VALUE;
        }

        {
            std::lock_guard<std::mutex> lk(g_dirMutex);
            for (auto* ctx : g_allDirs) {
                if (ctx->hDir != INVALID_HANDLE_VALUE) {
                    CancelIoEx(ctx->hDir, nullptr);
                    CloseHandle(ctx->hDir);
                    ctx->hDir = INVALID_HANDLE_VALUE;
                }
            }
        }

        if (g_DebounceWakeEvent) {
            CloseHandle(g_DebounceWakeEvent);
            g_DebounceWakeEvent = NULL;
        }

        return NO_ERROR;
    }

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    return NO_ERROR;
}



// 主服务，windows接口
VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    // 整体等待2秒各种系统资源加载
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    // ========= 1. 切换工作目录（允许失败，但必须最早） =========
    InitExeRootPath();
    if (!g_exeRoot.empty()) {
        SetCurrentDirectoryW(g_exeRoot.c_str());
    }

    // ========= 冷启动后清理  =========
    // a1. 强制清空所有运行态容器
    {
        std::lock_guard<std::mutex> lk(g_dirMutex);
        g_allDirs.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        g_eventCache.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_renameMutex);
        g_renamePending.clear();
    }

    // 初始化缓存处理时间窗口统一300毫秒
    PushQueueCtx.nextTimeInWindow = 300;
    WriteLogCtx.nextTimeInWindow = 300;
    WriteErrorCtx.nextTimeInWindow = 300;
    WriteMergeLogCtx.nextTimeInWindow = 300;
    WriteFileSyncLogCtx.nextTimeInWindow = 300;

    // a2. 明确复位全局运行标志
    g_running = false;
    // a3. 明确复位 shutdown 标志
    g_shutdownFastExit.store(false, std::memory_order_relaxed);
    // a4. 断开UNC连接，也给后面一点时间处理
    StopUNCSession();
	// a5. 清理运行检查文件
    deleteRunningCheckFile();

    // ========= 2. 注册 Service Control Handler =========
    g_StatusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceCtrlHandlerEx, nullptr);
    if (!g_StatusHandle) {
        // SCM 无法再接收任何状态，必须直接退出
        loadGeneralConfigErrorMsg.insert(L"注册 Service Control Handler 失败");
        return;
    }

    // ========= 3. 报告 START_PENDING（第一次） =========
    ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;  // 在这里提前注册 PRESHUTDOWN
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 1;
    g_ServiceStatus.dwWaitHint = 60000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // ========= 4. 加载全局配置（允许写日志） =========
    LoadGeneralConfig();

    // 优化后这里变成1
    //g_ServiceStatus.dwCheckPoint = 0;
    // 优化后这里去除了
    //g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    // ========= 5. 延迟启动（修改到放入异步触发，主进程不阻塞） =========
    g_ServiceStatus.dwCheckPoint++;
	g_ServiceStatus.dwWaitHint = (serviceDelayStartInSeconds + 3) * 1000;  //给异步时间为监听目录延迟秒数+3秒
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    // Sleep(serviceDelayStartInSeconds * 1000);
    // 【新增】：异步前额外报告仍在启动，避免 Failure Actions 早触发
    g_ServiceStatus.dwCheckPoint++;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // ========= 6. 初始化运行标志 =========
    // 显式设置全局运行状态，声明变量时默认值也是true，只有在正常退出时才会显式设置false
    g_running = true;


    // ========= 7. 创建 StopEvent（必须早于 RUNNING） =========
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_ServiceStopEvent) {
        loadGeneralConfigErrorMsg.insert(L"创建 ServiceStopEvent 失败");
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }


    // ========= 8. 创建防抖事件 =========
    // 第二个FALSE代表auto-reset
    g_DebounceWakeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_DebounceWakeEvent) {
        loadGeneralConfigErrorMsg.insert(L"创建 DebounceWakeEvent 失败");
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }


    // ========= 9. 创建 IOCP（关键资源） =========
    g_hCompPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!g_hCompPort) {
        loadGeneralConfigErrorMsg.insert(L"无法创建 IOCP 句柄");
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        // 被优化
        // g_ServiceStatus.dwServiceSpecificExitCode = 0;
        // g_ServiceStatus.dwCheckPoint = 0;
        // g_ServiceStatus.dwWaitHint = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    // ========= 10. 明确告诉 SCM：仍在启动 =========
    g_ServiceStatus.dwCheckPoint++;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);


    std::thread tIOCP;
    std::thread tDebounce;
    try {
        // ========= 11. 业务初始化（属于启动期） =========
        std::wstring wmsg = L"========================================程序开始运行========================================";
        WriteLog(wmsg, 5);

		// 冷启动时在同步流程里获取硬件信息可能导致服务无法启动，报2186无响应，丢入异步线程
        std::thread hardwareLogThread([]() {
            try {
                // std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                WriteLog(GetMotherboardModel() + L"    ||    " + CpuModelString() + L"    ||    " + GetMemoryType() + L" " + GetPhysicalMemoryTotal(false) + L"（"
                    + GetPhysicalMemoryTotal(true) + L"）" + L" + " + GetVirtualMemoryTotal(), 5);
            }
            catch (const std::exception& e) {
                WriteError(L"硬件日志异常：" + ToWide(e.what()), g_emptyWString, 4);
            }
            catch (...) {
                WriteError(L"硬件日志未知异常", g_emptyWString, 4);
            }
            });
        hardwareLogThread.detach();

        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        // 加载全局配置时积累下来的日志，全局配置必须先加载，放在全局变量里，这里打印
        if (!loadGeneralConfigMsg.empty()) {
            for (const auto& line : loadGeneralConfigMsg) {
                WriteLog(line, 5);
            }
            loadGeneralConfigMsg.clear();
        }
        if (!loadGeneralConfigErrorMsg.empty()) {
            for (const auto& line : loadGeneralConfigErrorMsg) {
                WriteError(line, cfgPath + L"/" + generalCFG, 4);
            }
            loadGeneralConfigErrorMsg.clear();
        }

        
        // 加载计数器d
        InitGlobalEventId();
        // 启动SMB会话
        LoadCredentialConfig(); // 加载监听注册所需的凭证，在LoadListenConfig时用到

        // 新增：更新 CheckPoint，告诉 SCM 还在启动中
        // g_ServiceStatus.dwCheckPoint = 2;
        // SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        try {
            fs::create_directories(cfgPath);
        }
        catch (const std::exception& e) {
            std::wstring wmsg = L"创建目录失败：" + ToWide(e.what());
            WriteError(wmsg, g_emptyWString, 4);
        }

        LoadIgnoreFilters(); // 加载过滤规则


        // 【异步执行】：不阻塞主线程，立即返回
        std::thread asyncInit([]() {
            if (g_shutdownFastExit.load(std::memory_order_relaxed)) return;  // 立即检查
            // 延迟启动，每400ms检查一次是否被停止中断，g_shutdownFastExit现在只要不是启用快速启动，都会被打上true
			int interval = 400; // ms
            for (int i = 0; i < serviceDelayStartInSeconds * 1000 / interval; ++i) {  // 400ms 轮询
                if (g_shutdownFastExit.load(std::memory_order_relaxed)) {
                    WriteLog(L"异步等待初始化被停止服务中断", 5);
                    return;  // 立即终止
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            }
            // std::this_thread::sleep_for(std::chrono::milliseconds(serviceDelayStartInSeconds * 1000));
            // 0. 此时网络应该就绪了
            // 0. 网络就绪前检查
            if (g_shutdownFastExit.load(std::memory_order_relaxed)) return;
            StartUNCSession();
            // 1. 加载监听前检查
            if (g_shutdownFastExit.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            LoadListenConfig();
            // 2. 调试打印前检查
            if (g_shutdownFastExit.load(std::memory_order_relaxed)) return;
            if (g_debugModeOnLoad) {
                logCurrentAllLoadedFolders();
                logCurrentLoadedExcludedFolders();
                logCurrentDirPairs(SYNC_DIR_PAIR);     // 正式监听目录对
                logCurrentDirPairs(SYNC_DIR_PAIR_RETRY); // 重试监听目录对
            }
            // 3. 初始化前检查
            if (g_shutdownFastExit.load(std::memory_order_relaxed)) return;
            InitSyncDirPairOnLoad();

            // 只有在服务真正可工作后，写入运行态检查文件
            if (g_shutdownFastExit.load(std::memory_order_relaxed)) return;
            InitRunningCheckFile();

            // 最终检查
            if (g_shutdownFastExit.load(std::memory_order_relaxed)) return;

            });
        // 立即 detach，让线程在后台独立运行，不阻塞主线程
        asyncInit.detach();

        // ========= 12. 启动工作线程 =========
        try {
            tIOCP = std::thread(IOCPWorker);
            tDebounce = std::thread(DebounceWorker);
        }
        catch (...) {
            g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            g_ServiceStatus.dwWin32ExitCode = ERROR_NOT_ENOUGH_MEMORY;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
            return;
        }

        // ========= 13. 现在，才允许进入 RUNNING ========= 从同步流程移入异步，等待监听目录全部加载完毕后打标记
        // g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
        if (g_shutdownFastExit.load(std::memory_order_relaxed)) return;
        g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
        g_ServiceStatus.dwCheckPoint = 0;
        g_ServiceStatus.dwWaitHint = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    }
    catch (...) {
        WriteError(L"初始化异常，强制 STOPPED", L"", 4);
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    }


    // ========= 14. 主线程阻塞，等待 STOP =========
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);

    // ========= 持久化事件ID  =========
    SaveGlobalEventId();
    StopUNCSession();

	// 强制把缓存的日志写入文件
    WriteLogToFile();
    WriteErrorToFile();

    // 修复：在调用 WaitForSingleObject 前增加空指针保护
    // if (g_ServiceStopEvent != NULL && g_ServiceStopEvent != INVALID_HANDLE_VALUE)
    // {
    //     WaitForSingleObject(g_ServiceStopEvent, INFINITE);
    // }

    // ========= 15. 回收线程 =========
    if (tIOCP.joinable()) tIOCP.join();
    if (tDebounce.joinable()) tDebounce.join();
    // 【新增】如果是关机路径，立即向 SCM 汇报 STOPPED
    if (g_shutdownFastExit.load(std::memory_order_relaxed)) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwWin32ExitCode = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    }

    // 原IOCP最后的清理工作移到这里来做（但 Handler 已处理句柄关闭，这里只清容器）
    {
        std::lock_guard<std::mutex> lk(g_dirMutex);
        for (auto* ctx : g_allDirs) {
            delete ctx;  // 只剩删除对象
        }
        g_allDirs.clear();
    }

    // 手动 STOP 场景，正常汇报 STOPPED
    // 显式告知 Windows 服务已停止
    if (!g_shutdownFastExit.load(std::memory_order_relaxed)) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwControlsAccepted = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    }

    // 退出时清理防抖事件
    if (g_DebounceWakeEvent) {
        CloseHandle(g_DebounceWakeEvent);
        g_DebounceWakeEvent = NULL;
    }
    // 最后关闭句柄
    if (g_hCompPort != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hCompPort);
        g_hCompPort = INVALID_HANDLE_VALUE;
    }

}



// 等待服务停止完成，返回是否成功停止（true=已停止，false=超时或失败）
bool WaitForServiceStopped() {
    bool running = checkRunningCheckFile();
    if (running) {
        for (int i = 0; i < 60; ++i) {  // 最多 30 秒
            if (!checkRunningCheckFile()) {
                return true;
            }
            Sleep(500);
        }
    }
    else {
        // 如果一开始就是无运行文件，直接返回true表示服务已经停止
        return true;
    }
    return false;
}


// 供计划任务调用，用于在异常退出状态后停止服务，后交由正常流程再次启动服务
void StopServiceByScheduledTask() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(
        scm,
        SERVICE_NAME,
        SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS
    );
    SERVICE_STATUS status{};

    try {
        // 防止一些非常异常的情况，如在某次windows启动中启动混乱，使用这一次的启动把服务硬性关了
        // 运行文件也删除，期望下一次启动能恢复正常初始状态，try尝试，catch清理
        Sleep(500);

        if (!svc) {
            CloseServiceHandle(scm);
            return;
        }

        // 入口第一时间查询当前状态
        if (!QueryServiceStatus(svc, &status)) {
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return;
        }
        // 如果状态为 RUNNING 或 START_PENDING，则发送停止指令
        //if (status.dwCurrentState == SERVICE_RUNNING || status.dwCurrentState == SERVICE_START_PENDING) {
        if (checkRunningCheckFile()) {

            // WriteLog(L"检测到Windows启用快速启动，处理关机后冷启动残留数据", 5);
            // WriteLogToFile();

            ControlService(svc, SERVICE_CONTROL_STOP, &status);

            // 等待停止完成（最多 30 秒）
            for (int i = 0; i < 60; ++i) {
                QueryServiceStatus(svc, &status);
                if (status.dwCurrentState == SERVICE_STOPPED)
                    break;
                Sleep(500);
            }

            // 解决windows对于服务的处理机制，核心代码部分
            // 停止后立即重启服务，这个代码只适用于启用快速启动时异常退出场景，在计划任务内只停，因为计划任务的STOP会把主进程的服务注册直接中止
            // 场景：启用快速启动时，关机+冷启动走这里if
            // 场景：禁用快速启动时的关机+冷启动、无论是否禁用快速启动时的热启动、手动STOP，都不会走这里if，正常走int main里的起服务流程
            bool stopped = WaitForServiceStopped();
            Sleep(500);
            if (!stopped) {
                // 超时：强制杀进程（用 taskkill /F /IM XYFileSync.exe）
                WriteLog(L"停服务超时，强制杀进程", 4);
                // system("taskkill /F /IM XYFileSync /T >nul 2>&1");
                std::wstring cmd = std::format(L"taskkill /F /IM {} /T >nul 2>&1", SERVICE_NAME );
                int ret = _wsystem(cmd.c_str());

                Sleep(1000);  // 给时间杀掉
                stopped = true;
            }
            if (stopped) StartServiceW(svc, 0, nullptr);
        }

        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
    }
    catch (...) {
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
		deleteRunningCheckFile();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return;
	}
}



// 供计划任务调用，用于在异常退出状态后停止服务，后交由正常流程再次启动服务
void RestartServiceByScheduledTask() {
    Sleep(500);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;

    SC_HANDLE svc = OpenServiceW(
        scm,
        SERVICE_NAME,
        SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS
    );

    if (!svc) {
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS status{};

    // 尝试停止（不关心当前状态）
    ControlService(svc, SERVICE_CONTROL_STOP, &status);

    // 等待停止完成（最多 30 秒）
    for (int i = 0; i < 60; ++i) {
        QueryServiceStatus(svc, &status);
        if (status.dwCurrentState == SERVICE_STOPPED)
            break;
        Sleep(500);
    }

    Sleep(500);

    // 再次启动
    StartServiceW(svc, 0, nullptr);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}


// 注册计划任务 XYFileSync_Restart，旧版本直接采用schtasks /create /f方式，不支持两个电源选项的显式禁用，不用这个方法
void RegisterRestartTaskWIN() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // 先检查任务是否存在（用 /Query + >nul 2>&1 隐藏输出）
    int taskExists = system("schtasks /Query /TN \"XYFileSync_Restart\" >nul 2>&1");
    if (taskExists == 0) {  // 0 表示存在
        system("schtasks /Delete /TN \"XYFileSync_Restart\" /F >nul 2>&1");  // 删除并隐藏输出
    }

    // 创建任务（原有命令，隐藏输出避免小报错）
    // /sc onlogon为用户登录时触发、/ru SYSTEM为不依赖用户登录、/sc onstart为对SYSTEM启动
    std::wstring cmd = L"schtasks /create /f "
        L"/sc onlogon "
        L"/ru SYSTEM "
        L"/rl HIGHEST "
        L"/tn \"XYFileSync_Restart\" "
        L"/tr \"\\\"" + std::wstring(exePath) + L"\\\" --restart\" "
        // 不设置时默认不延迟
        // L"/DELAY 0000:00 "
        L"/np "                                 // 非交互模式
        L">nul 2>&1";                           // 隐藏所有输出
    _wsystem(cmd.c_str());
    std::cout << "计划任务XYFileSync_Restart安装成功。" << std::endl;
}

// 注册计划任务 XYFileSync_Restart，XML版本，因为支持两个电源选项的显式禁用
void RegisterRestartTask() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // 先检查任务是否存在（用 /Query + >nul 2>&1 隐藏输出）
    int taskExists = system("schtasks /Query /TN \"XYFileSync_Restart\" >nul 2>&1");
    if (taskExists == 0) {  // 0 表示存在
        system("schtasks /Delete /TN \"XYFileSync_Restart\" /F >nul 2>&1");  // 删除并隐藏输出
    }

    // 生成 XML 文件内容（显式禁用电源选项）
    std::wstring xml = L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo />\r\n"
        L"  <Triggers>\r\n"
        L"    <LogonTrigger>\r\n"
        // 不设置时默认不延迟
        L"      <Delay>PT1S</Delay>\r\n"  // 延迟 x 秒
        L"    </LogonTrigger>\r\n"
        L"  </Triggers>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <UserId>S-1-5-18</UserId>\r\n"  // SYSTEM
        L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"  // 显式禁用“电池时停止”
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"  // 显式禁用“切换电池时停止”
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
        L"    <IdleSettings>\r\n"
        L"      <StopOnIdleEnd>true</StopOnIdleEnd>\r\n"
        L"      <RestartOnIdle>false</RestartOnIdle>\r\n"
        L"    </IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <WakeToRun>false</WakeToRun>\r\n"
        L"    <ExecutionTimeLimit>P3D</ExecutionTimeLimit>\r\n"
        L"    <Priority>7</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>\"" + std::wstring(exePath) + L"\"</Command>\r\n"
        L"      <Arguments>--restart</Arguments>\r\n"
        L"    </Exec>\r\n"
        L"  </Actions>\r\n"
        L"</Task>\r\n";

    // 写 XML 到临时文件
    std::wstring xmlFile = g_exeRoot + L"\\task.xml";
    std::wofstream ofs(xmlFile);
    ofs << xml;
    ofs.close();

    // 用 XML 创建任务
    std::wstring cmd = L"schtasks /create /tn \"XYFileSync_Restart\" /xml \"" + xmlFile + L"\" /f >nul 2>&1";
    _wsystem(cmd.c_str());

    // 删除临时 XML
    fs::remove(xmlFile);

    std::cout << "计划任务XYFileSync_Restart安装成功。" << std::endl;
}

// 删除计划任务 XYFileSync_Restart
void DeleteRestartTask() {
    _wsystem(L"schtasks /delete /f /tn \"XYFileSync_Restart\"");
    std::cout << "计划任务XYFileSync_Restart删除成功。" << std::endl;
}


// --- 安装与移除函数 ---
void InstallService() {
    SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    Sleep(500);
    SC_HANDLE svc = CreateServiceW(
        scm,
        SERVICE_NAME,
        SERVICE_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        nullptr, nullptr, nullptr, nullptr, nullptr
    );

    if (!svc) {
        CloseServiceHandle(scm);
        return;
    }

    // 1. 设置描述
    SERVICE_DESCRIPTIONW sd{};
    sd.lpDescription = (LPWSTR)L"XY的文件自动静默同步服务，用于后台捕获windows指定配置路径下的文件变化，并自动同步到远程目录时刻保持同步";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sd);

    // 2. 设置 PRESHUTDOWN
    SERVICE_PRESHUTDOWN_INFO psi{};
    psi.dwPreshutdownTimeout = 3000;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_PRESHUTDOWN_INFO, &psi);

    // 3. ⭐ 关键：设置 Failure Actions（失败后 6 秒重启）
    SC_ACTION actions[1]{};
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 3000; // 6 秒

    SERVICE_FAILURE_ACTIONSW sfa{};
    sfa.dwResetPeriod = 0;  // 立即重置计数（避免连续失败）
    sfa.cActions = 1;
    sfa.lpsaActions = actions;

    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa );

    std::cout << "服务XYFileSync安装成功。" << std::endl;

	// 注册计划任务、重置运行文件
    RegisterRestartTask();
    deleteRunningCheckFile();

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}


void UninstallService() {
    SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) return;

    SC_HANDLE schService = OpenServiceW(schSCManager, SERVICE_NAME, DELETE);
    if (schService) {
        if (DeleteService(schService)) {
            std::cout << "服务移除成功。" << std::endl;
        }
		// 删除计划任务、重置运行文件
        DeleteRestartTask();
        deleteRunningCheckFile();

        CloseServiceHandle(schService);
    }
    CloseServiceHandle(schSCManager);
}



// --- 入口函数 ---

int main(int argc, char* argv[]) {

    try {
        InitExeRootPath();
        // ===== 计划任务调用 =====
        if (argc > 1 && strcmp(argv[1], "--restart") == 0) {
            Sleep(500);
            // 探测状态为RUNNING或PENDINGSTART，则立即停服务
			// ===================== 重要！！！ =====================
            // 必须要控制计划任务能在int main之前就触发
            StopServiceByScheduledTask();
            // 强制退出，避免继续 Dispatcher，因为这是计划任务一次额外的调用，和重启后服务常规触发的调用是两次事件
            return 0;
        }

        if (argc > 1) {
            if (strcmp(argv[1], "--install") == 0) {
                InstallService();
                return 0;
            }
            else if (strcmp(argv[1], "--uninstall") == 0) {
                UninstallService();
                return 0;
            }
        }

        // ===================== 重要！！！ =====================
        // 控制必须要晚于计划任务触发，让计划任务先做
        bool stopped = WaitForServiceStopped();
        Sleep(4000);
        SERVICE_TABLE_ENTRYW ServiceTable[] = { {(LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain}, {NULL, NULL} };
        if (!StartServiceCtrlDispatcherW(ServiceTable)) {
            DWORD err = GetLastError();
            if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                std::cout << "请通过服务管理器或管理脚本启动此程序。" << std::endl;
            }
        }
    }
    catch (const std::exception& e) {
        std::wstring wmsg = L"进程发生未处理的异常：" + ToWide(e.what());
        WriteError(wmsg, g_emptyWString, 4);
    }
    catch (...) {
        std::wstring wmsg = L"进程发生未知系统错误";
        WriteError(wmsg, g_emptyWString, 4);
        SaveGlobalEventId();
        return 1;
    }
    return 0;

}



