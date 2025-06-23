#include "BreadcrumbLogger.h"
#include <fstream>
#include <iomanip>

std::vector<Breadcrumb> BreadcrumbLogger::m_breadcrumbs;
std::mutex BreadcrumbLogger::m_mutex;

void BreadcrumbLogger::Add(const std::string& category, 
                          const std::string& message,
                          const std::map<std::string, std::string>& context)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_breadcrumbs.size() >= MAX_BREADCRUMBS) {
        m_breadcrumbs.erase(m_breadcrumbs.begin());
    }
    
    Breadcrumb crumb;
    crumb.category = category;
    crumb.message = message;
    crumb.context = context;
    crumb.timestamp = std::time(nullptr);
    m_breadcrumbs.push_back(crumb);
}

std::vector<Breadcrumb> BreadcrumbLogger::GetBreadcrumbs()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_breadcrumbs;
}

void BreadcrumbLogger::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_breadcrumbs.clear();
}

void BreadcrumbLogger::DumpToFile(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream file(filename);
    
    if (!file.is_open()) return;
    
    for (const auto& crumb : m_breadcrumbs) {
        std::tm* tm = std::localtime(&crumb.timestamp);
        char timeStr[20];
        std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", tm);
        
        file << "[" << timeStr << "] "
             << crumb.category << ": " 
             << crumb.message << "\n";
        
        for (const auto& [key, value] : crumb.context) {
            file << "  " << key << " = " << value << "\n";
        }
        file << "\n";
    }
}
