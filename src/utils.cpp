#include "rangoons.h"
#include <sstream>
#include <iomanip>
#include <random>
#include <cctype>

std::string url_decode(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i=0; i<s.size(); ++i) {
        if (s[i] == '+') out.push_back(' ');
        else if (s[i] == '%' && i+2 < s.size()) {
            int val = 0;
            std::istringstream iss(s.substr(i+1,2));
            iss >> std::hex >> val;
            out.push_back(static_cast<char>(val));
            i += 2;
        } else out.push_back(s[i]);
    }
    return out;
}

std::unordered_map<std::string,std::string> parse_form(const std::string& body) {
    std::unordered_map<std::string,std::string> m;
    size_t start=0;
    while (start < body.size()) {
        auto amp = body.find('&', start);
        std::string pair = body.substr(start, amp==std::string::npos ? std::string::npos : amp - start);
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = url_decode(pair.substr(0, eq));
            std::string v = url_decode(pair.substr(eq+1));
            m[k]=v;
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return m;
}

std::unordered_map<std::string,std::string> parse_cookies(const std::string& h) {
    std::unordered_map<std::string,std::string> m;
    size_t i=0;
    while (i < h.size()) {
        while (i < h.size() && (h[i]==' '||h[i]==';')) ++i;
        size_t eq = h.find('=', i);
        if (eq == std::string::npos) break;
        size_t end = h.find(';', eq);
        std::string k = h.substr(i, eq-i);
        std::string v = h.substr(eq+1, (end==std::string::npos? h.size(): end) - (eq+1));
        m[k]=v;
        if (end == std::string::npos) break;
        i = end+1;
    }
    return m;
}

std::string html_escape(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c: s) {
        switch(c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string gen_cart_id() {
    static const char* hex = "0123456789abcdef";
    std::random_device rd; std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a=dist(rng), b=dist(rng);
    std::string s(32, '0');
    for (int i=0;i<16;i++) s[i] = hex[(a>>(i*4))&0xf];
    for (int i=0;i<16;i++) s[16+i] = hex[(b>>(i*4))&0xf];
    return s;
}

std::string money_fmt(int cents) {
    std::ostringstream os;
    os << "Rs " << (cents/100) << "." << std::setw(2) << std::setfill('0') << (cents%100);
    return os.str();
}
