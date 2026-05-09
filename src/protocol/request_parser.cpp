#include "protocol/request_parser.h"
#include "core/server_config.h"
#include "utils/url_rewriter.h"
#include <sstream>


RequestInfo RequestParser::parse(const std::string &request_data)
{
    RequestInfo info;
    std::istringstream ss(request_data);
    if (!(ss >> info.method >> info.raw_uri >> info.version))
    {
        return info;
    }

    info.is_http = (info.version.find("HTTP/") == 0);

    if (ServerConfig::getToken().empty())
    {
        info.is_authorized = true;
    }

    info.clean_uri = info.raw_uri;
    size_t qpos = info.raw_uri.find('?');
    if (qpos != std::string::npos)
    {
        std::string query_str = info.raw_uri.substr(qpos + 1);
        std::string base_uri = info.raw_uri.substr(0, qpos);
        
        std::vector<std::string> params_vec = split(query_str, '&');
        std::vector<std::string> filtered_params;
        
        std::string server_token = ServerConfig::getToken();

        for (const auto &p : params_vec)
        {
            size_t eq_pos = p.find('=');
            if (eq_pos != std::string::npos)
            {
                std::string key = p.substr(0, eq_pos);
                std::string val = p.substr(eq_pos + 1);
                info.params[key] = val;

                if (key == "token")
                {
                    if (val == server_token)
                    {
                        info.is_authorized = true;
                    }
                    continue;
                }
            }
            filtered_params.push_back(p);
        }

        info.clean_uri = base_uri;
        if (!filtered_params.empty())
        {
            info.clean_uri += "?";
            for (size_t i = 0; i < filtered_params.size(); ++i)
            {
                if (i != 0) info.clean_uri += "&";
                info.clean_uri += filtered_params[i];
            }
        }
    }

    size_t path_start = info.clean_uri.find("/rtp/");
    if (path_start == std::string::npos) {
        path_start = info.clean_uri.find("/tv/");
    }

    if (path_start != std::string::npos) {
        std::string sub_path = info.clean_uri.substr(path_start);
        URLRewriter::rewrite_path(sub_path, info.upstream_url);
    }

    return info;
}

std::vector<std::string> RequestParser::split(const std::string &str, char delimiter)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    size_t end = str.find(delimiter);

    while (end != std::string::npos)
    {
        tokens.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delimiter, start);
    }
    tokens.push_back(str.substr(start, end));

    return tokens;
}
