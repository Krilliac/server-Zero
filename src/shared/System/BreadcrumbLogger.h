#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <ctime>

struct Breadcrumb
{
    std::string category;
    std::string message;
    std::map<std::string, std::string> context;
    time_t timestamp;
};

class BreadcrumbLogger
{
public:
    static void Add(const std::string& category, 
                   const std::string& message,
                   const std::map<std::string, std::string>& context = {});
    
    static std::vector<Breadcrumb> GetBreadcrumbs();
    static void Clear();
    static void DumpToFile(const std::string& filename);

private:
    static std::vector<Breadcrumb> m_breadcrumbs;
    static std::mutex m_mutex;
    static constexpr size_t MAX_BREADCRUMBS = 1000;
};
