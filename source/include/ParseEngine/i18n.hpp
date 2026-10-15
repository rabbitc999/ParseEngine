/** @file i18n.hpp
 * @brief ParseEngine 静态三语消息与显示语言选择，独立于代码生成目标语言。
 */
#ifndef PARSEENGINE_I18N_HPP
#define PARSEENGINE_I18N_HPP
#include "ParseEngine/tool_console.hpp"
#include <cstring>
#include <cstdlib>
namespace ParseEngine { namespace I18n {
/** @brief 支持的显示语言。 */
enum class Language { Zh, En, Ja, Auto };
/** @brief 识别短名称和区域串。 @param value 语言名称。 @return 匹配语言或 Auto。 */
inline Language parse(const char *value){
    if(!value || !*value)return Language::Auto;
    const auto lower=[](char c){return c>='A'&&c<='Z'?char(c-'A'+'a'):c;};
    if(std::strlen(value)>=2 && (!value[2] || value[2]=='_' || value[2]=='-' || value[2]=='.' || value[2]=='@')){
        if(lower(value[0])=='z' && lower(value[1])=='h')return Language::Zh;
        if(lower(value[0])=='j' && lower(value[1])=='a')return Language::Ja;
        if(lower(value[0])=='e' && lower(value[1])=='n')return Language::En;
    }
    return Language::Auto;
}
/** @brief 查询环境和系统显示语言。 @return 具体语言。 */
inline Language detect(){
    const char *setting=std::getenv("PARSEENGINE_LANG");
    if(setting && *setting && std::strcmp(setting,"auto")!=0){const auto value=parse(setting);return value==Language::Auto?Language::En:value;}
    for(const char *name:{"LC_ALL","LC_MESSAGES","LANG"}){
        const char *value=std::getenv(name);
        if(value && *value){const auto language=parse(value);return language==Language::Auto?Language::En:language;}
    }
#ifdef _WIN32
    const auto id=PRIMARYLANGID(GetUserDefaultUILanguage());
    if(id==LANG_CHINESE)return Language::Zh;
    if(id==LANG_JAPANESE)return Language::Ja;
#endif
    return Language::En;
}
/** @brief 当前显式选择；Auto 每次查询系统。 */
inline Language selected=Language::Auto;
/** @brief 获取当前显示语言。 @return 具体语言。 */
inline Language language(){return selected==Language::Auto?detect():selected;}
/** @brief 静态消息条目。 */
struct Message { const char *zh; const char *en; const char *ja; };
#include "ParseEngine/i18n_messages.hpp"
/** @brief 查询精确消息键，缺失时保留原文。 @param original 中文键。 @return 静态 UTF-8 消息。 */
inline const char *text(const char *original){
    const auto chosen=language();if(chosen==Language::Zh)return original;
    std::size_t low=0,high=sizeof(messages)/sizeof(messages[0]);
    while(low<high){const auto mid=low+(high-low)/2;const int order=std::strcmp(original,messages[mid].zh);
        if(!order)return chosen==Language::Ja?messages[mid].ja:messages[mid].en;
        if(order<0)high=mid;else low=mid+1;}
    return original;
}
/** @brief 翻译诊断的完整键或已登记前缀；用户路径和源码保持原样。
 * @param original 完整诊断。 @return 拥有型显示文本。
 */
inline std::string diagnostic(const std::string &original){
    const char *exact=text(original.c_str());if(exact!=original.c_str())return exact;
    if(language()==Language::Zh)return original;
    const Message *found=nullptr;std::size_t length=0;
    for(const auto &entry:messages){const auto size=std::strlen(entry.zh);
        if(size>length && original.compare(0,size,entry.zh)==0){found=&entry;length=size;}}
    if(!found)return original;
    return std::string(language()==Language::Ja?found->ja:found->en)+original.substr(length);
}
/** @brief 提取显示选项，保留 --lang 的代码生成含义。
 * @param args UTF-8 参数，成功后移除显示选项。 @param error 失败说明。 @return 选项有效时为真。
 */
inline bool configure(std::vector<std::string> &args,std::string &error){
    for(std::size_t i=1;i<args.size();){
        if(args[i]=="--")break;
        const bool separate=args[i]=="--ui-lang";
        if(!separate && args[i].compare(0,10,"--ui-lang=")!=0){++i;continue;}
        if(separate && i+1>=args.size()){error=text("--ui-lang 后需要 zh、en、ja 或 auto");return false;}
        const std::string value=separate?args[i+1]:args[i].substr(10);
        if(value!="zh"&&value!="en"&&value!="ja"&&value!="auto"){error=std::string(text("不支持的显示语言："))+value;return false;}
        selected=value=="auto"?Language::Auto:parse(value.c_str());
        args.erase(args.begin()+i,args.begin()+i+(separate?2:1));
    }
    return true;
}
}}
#define PE_TEXT(original) (::ParseEngine::I18n::text(original))
#endif
