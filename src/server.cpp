#include "rangoons.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <sqlite3.h>

#include <cstring>
#include <string>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <vector>
#include <cstdlib>
#include <iomanip>

// ---------------- Globals -----------------
static DB g_db;

// Your WhatsApp owner number for wa.me (no + or leading 00)
// You said: 00923001555681  -> wa.me/923001555681
static const char* OWNER_WA_NUM = "923001555681";

// ---------------- Helpers -----------------

// Minimal URL encoder for query strings
static std::string url_encode(const std::string& s) {
    std::ostringstream o;
    o << std::uppercase << std::hex;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c=='-' || c=='_' || c=='.' || c=='~') {
            o << c;
        } else if (c == ' ') {
            o << "%20";
        } else {
            o << '%' << std::setw(2) << std::setfill('0') << int(c);
        }
    }
    return o.str();
}

// Minimal JSON escaper for strings
static std::string json_escape(const std::string& s) {
    std::string out; out.reserve(s.size()+8);
    for (unsigned char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { // control
                    char buf[7]; std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// price with 20% margin, rounded up to cents
static inline int apply_margin_cents(int base_cents) {
    // ceil(base * 1.20) in integer cents
    // (x*120 + 99) / 100 performs integer ceil
    return (base_cents * 120 + 99) / 100;
}

// ---------------- Small HTTP parsing -----------------
struct HttpRequest {
    std::string method;
    std::string target;
    std::map<std::string,std::string> headers;
    std::string body;
};

static std::string trim(const std::string& s){
    size_t a=0,b=s.size();
    while (a<b && (s[a]==' '||s[a]=='\t'||s[a]=='\r'||s[a]=='\n')) ++a;
    while (b>a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\r'||s[b-1]=='\n')) --b;
    return s.substr(a,b-a);
}
static std::string tolower_str(std::string v){ for(char& c:v) c=std::tolower((unsigned char)c); return v; }

static bool recv_all(int fd, std::string& out) {
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    out.append(buf, buf+n);

    auto hdr_end = out.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return true;

    // parse headers quickly to read body length
    std::string headers = out.substr(0, hdr_end+4);
    std::istringstream hs(headers);
    std::string line; std::map<std::string,std::string> hmap;
    std::getline(hs, line); // request line
    while (std::getline(hs, line)) {
        if (line == "\r") break;
        auto colon = line.find(':');
        if (colon!=std::string::npos) {
            std::string k = tolower_str(trim(line.substr(0,colon)));
            std::string v = trim(line.substr(colon+1));
            if (!v.empty() && v.back()=='\r') v.pop_back();
            hmap[k]=v;
        }
    }
    size_t need = 0;
    if (hmap.count("content-length")) {
        need = (size_t)std::stoul(hmap["content-length"]);
    }
    size_t have = out.size() - (hdr_end+4);
    while (have < need) {
        n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf+n);
        have += n;
    }
    return true;
}

static bool parse_request(const std::string& raw, HttpRequest& req) {
    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return false;
    std::string head = raw.substr(0, hdr_end);
    std::istringstream ss(head);
    std::string line;
    if (!std::getline(ss, line)) return false;
    if (!line.empty() && line.back()=='\r') line.pop_back();
    { std::istringstream rl(line); rl >> req.method >> req.target; }
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon!=std::string::npos) {
            std::string k = tolower_str(trim(line.substr(0, colon)));
            std::string v = trim(line.substr(colon+1));
            req.headers[k]=v;
        }
    }
    req.body = raw.substr(hdr_end+4);
    return true;
}

static void send_response(int cfd, int code, const std::string& status,
                          const std::string& content_type,
                          const std::string& body,
                          const std::string& set_cookie="",
                          const std::string& location="") {
    std::ostringstream res;
    res << "HTTP/1.1 " << code << " " << status << "\r\n";
    res << "Content-Type: " << content_type << "\r\n";
    res << "Content-Length: " << body.size() << "\r\n";
    res << "Connection: close\r\n";
    if (!set_cookie.empty()) res << "Set-Cookie: " << set_cookie << "\r\n";
    if (!location.empty())   res << "Location: " << location << "\r\n";
    res << "\r\n" << body;
    auto s = res.str();
    send(cfd, s.data(), s.size(), 0);
}

// ---------------- HTML layout & pages -----------------
static std::string layout(const std::string& title, const std::string& body) {
    std::ostringstream os;
    os <<
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>" << html_escape(title) << "</title>"
"<style>"
"body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:#f7f7f8;color:#111}"
"header,footer{background:#111;color:#fff;padding:12px}"
"header a{color:#fff;text-decoration:none;margin-right:10px}"
"main{padding:16px;max-width:1000px;margin:0 auto}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px}"
".card{background:#fff;border-radius:14px;box-shadow:0 1px 4px rgba(0,0,0,.08);padding:12px;margin:8px 0;transition:transform .05s ease}"
".card:hover{transform:translateY(-2px)}"
".price{font-weight:700;color:#ff5a1f}"
"form{margin:0;display:inline-block}"
"input,button{padding:8px;border-radius:10px;border:1px solid #bbb}"
"button{cursor:pointer;background:#ff5a1f;border:0;color:#fff}"
"button:hover{filter:brightness(1.05)}"
"img{max-width:100%;border-radius:10px;aspect-ratio:1/1;object-fit:cover;background:#eee}"
".muted{color:#666;font-size:12px}"
"</style></head><body>"
"<header><b>Rangoons Shop</b> — "
"<a href='/'>Home</a><a href='/cart'>Cart</a>"
"</header>"
"<main>" << body << "</main>"
"<footer><span class='muted'>RangoonsCore e-commerce — PK nationwide delivery</span></footer>"
"</body></html>";
    return os.str();
}

static std::string page_home_html() {
    // Empty grid; client loads via /api/products (infinite scroll)
    std::ostringstream b;
    b << "<h2>Trending Products</h2><div class='grid' id='grid'></div>";
    // Sentinel + loader JS
    b << R"(
<script>
let off=0, busy=false, done=false;
async function loadMore(){
  if(busy||done) return; busy=true;
  const res=await fetch(`/api/products?sort=trending&offset=${off}&limit=24`);
  if(!res.ok){busy=false;return;}
  const items=await res.json();
  const grid=document.getElementById('grid');
  for(const p of items){
    const card=document.createElement('div'); card.className='card';
    const price = (p.display_price_cents/100).toLocaleString('en-PK', {maximumFractionDigits:0});
    card.innerHTML = `
      ${p.image_url? `<img src="${p.image_url}" alt="">`:``}
      <h3>${p.name}</h3>
      <div class="price">Rs ${price}</div>
      <form method="POST" action="/cart/add">
        <input type="hidden" name="product_id" value="${p.id}"/>
        <input type="number" name="qty" value="1" min="1" style="width:70px"/>
        <button type="submit">Add to Cart</button>
      </form>`;
    grid.appendChild(card);
  }
  off += items.length;
  if(items.length===0) done=true;
  busy=false;
}
const s=document.createElement('div'); s.id='sentinel'; document.body.appendChild(s);
const ob=new IntersectionObserver((e)=>{ if(e[0].isIntersecting) loadMore(); });
window.addEventListener('load', ()=>{ ob.observe(s); loadMore(); });
</script>
)";
    return layout("Rangoons — Trending", b.str());
}

static std::string page_cart_html(const std::string& cart_id) {
    auto items = g_db.get_cart_items(cart_id);
    int total=0;
    std::ostringstream b;
    b << "<h2>Your Cart</h2>";
    if (items.empty()) {
        b << "<p>Cart is empty.</p>";
    } else {
        b << "<div class='card'><ul>";
        for (auto& it: items) {
            int base = it.price_cents; // importer stores base price here
            int line = it.qty * apply_margin_cents(base);
            total += line;
            b << "<li>" << html_escape(it.name) << " × " << it.qty
              << " — " << html_escape(money_fmt(line)) << "</li>";
        }
        b << "</ul><b>Total: " << html_escape(money_fmt(total)) << "</b></div>";
        b << "<h3>Checkout</h3>"
          << "<form method='POST' action='/checkout'>"
          << "Name: <input name='name' required/> "
          << "Phone: <input name='phone' required/> "
          << "<br><br>Address:<br><input name='address' required style='width:100%'/><br><br>"
          << "<button type='submit'>Place Order</button></form>";
    }
    return layout("Cart", b.str());
}

// ---------------- Routing -----------------
static void handle_request(int cfd, const HttpRequest& in_req) {
    // Normalize method & split query
    HttpRequest req = in_req;
    for (char& c: req.method) c = std::toupper((unsigned char)c);
    std::string base = req.target, query;
    auto qpos = base.find('?');
    if (qpos != std::string::npos) { query = base.substr(qpos+1); base = base.substr(0, qpos); }
    req.target = base;

    // In-memory cart key (simple)
    std::string cart_id = "cart1";
    g_db.ensure_cart(cart_id);

    // Routes
    if (req.method=="GET" && (req.target=="/" || req.target=="/index.html")) {
        send_response(cfd, 200, "OK", "text/html; charset=utf-8", page_home_html());
        return;
    }

    if (req.method=="GET" && req.target=="/cart") {
        send_response(cfd, 200, "OK", "text/html; charset=utf-8", page_cart_html(cart_id));
        return;
    }

    if (req.method=="POST" && req.target=="/cart/add") {
        auto form = parse_form(req.body);
        int pid = std::stoi(form["product_id"]);
        int qty = std::max(1, std::stoi(form["qty"]));
        std::string err;
        if (!g_db.add_to_cart(cart_id, pid, qty, &err)) {
            send_response(cfd, 400, "Bad Request", "text/plain; charset=utf-8", "Failed to add to cart");
            return;
        }
        // UX: bounce back to home
        send_response(cfd, 303, "See Other", "text/plain; charset=utf-8", "", "", "/");
        return;
    }

    if (req.method=="POST" && req.target=="/checkout") {
        auto form = parse_form(req.body);
        auto items = g_db.get_cart_items(cart_id);

        int total=0;
        for (auto& it: items) {
            int line = it.qty * apply_margin_cents(it.price_cents);
            total += line;
        }

        Order o;
        o.customer_name = form["name"];
        o.phone         = form["phone"];
        o.address       = form["address"];
        o.total_cents   = total;

        int oid=0; std::string err;
        if (!g_db.create_order(o, &oid, &err)) {
            send_response(cfd, 400, "Bad Request", "text/plain; charset=utf-8", "Failed to place order");
            return;
        }

        // Update trending stats
        // Requires product_stats(product_id INTEGER PRIMARY KEY, sold_count INTEGER DEFAULT 0, ...)
        sqlite3* db = (sqlite3*)g_db.handle;
        for (auto& it: items) {
            // ensure row exists
            std::ostringstream ins; ins << "INSERT OR IGNORE INTO product_stats(product_id,sold_count) VALUES ("<<it.product_id<<",0);";
            sqlite3_exec(db, ins.str().c_str(), nullptr, nullptr, nullptr);
            // bump sold_count by qty
            std::ostringstream upd; upd << "UPDATE product_stats SET sold_count = sold_count + " << it.qty
                                        << " WHERE product_id=" << it.product_id << ";";
            sqlite3_exec(db, upd.str().c_str(), nullptr, nullptr, nullptr);
        }

        g_db.clear_cart(cart_id);

        // Build confirmation page with Click-to-WhatsApp to OWNER
        std::ostringstream w;
        w << "New order #" << oid << "%0A"
          << "Name: "   << url_encode(o.customer_name) << "%0A"
          << "Phone: "  << url_encode(o.phone)         << "%0A"
          << "Address: "<< url_encode(o.address)       << "%0A"
          << "Total: "  << url_encode(money_fmt(total))<< "%0A";

        std::ostringstream b;
        b << "<h2>Order Placed</h2>"
          << "<p>Order #"<<oid<<" — Total "<<html_escape(money_fmt(total))<<"</p>"
          << "<p>We will contact you on WhatsApp to confirm delivery.</p>"
          << "<p><a target='_blank' href='https://wa.me/"<< OWNER_WA_NUM <<"?text="<< w.str() <<"'>"
          << "<button>Send on WhatsApp</button></a></p>"
          << "<p><a href='/'>Continue shopping</a></p>";

        send_response(cfd, 200, "OK", "text/html; charset=utf-8", layout("Order Placed", b.str()));
        return;
    }

    // JSON API: /api/products?sort=trending&offset=0&limit=24
    if (req.method=="GET" && req.target=="/api/products") {
        // parse params (simple)
        int offset=0, limit=24;
        std::string sort="trending";
        if (!query.empty()) {
            auto kv = parse_form(query);
            if (kv.count("offset")) offset = std::max(0, std::stoi(kv["offset"]));
            if (kv.count("limit"))  limit  = std::min(200, std::max(1, std::stoi(kv["limit"])));
            if (kv.count("sort"))   sort   = kv["sort"];
        }
        // SQL: join with product_stats; order by sold_count desc
        sqlite3* db = (sqlite3*)g_db.handle;
        std::ostringstream sql;
        sql <<
            "SELECT p.id, p.name, p.image_url, "
            "COALESCE(p.base_price_cents, p.price_cents) AS base_cents, "
            "COALESCE(s.sold_count,0) AS sold "
            "FROM products p "
            "LEFT JOIN product_stats s ON s.product_id = p.id ";
        if (sort=="trending")
            sql << "ORDER BY sold DESC, p.id DESC ";
        else
            sql << "ORDER BY p.id DESC ";
        sql << "LIMIT " << limit << " OFFSET " << offset << ";";

        sqlite3_stmt* st=nullptr;
        std::ostringstream out;
        out << "[";
        bool first=true;
        if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &st, nullptr)==SQLITE_OK) {
            while (sqlite3_step(st)==SQLITE_ROW) {
                int id = sqlite3_column_int(st,0);
                std::string name = (const char*)sqlite3_column_text(st,1);
                std::string img  = (const char*)sqlite3_column_text(st,2);
                int base = sqlite3_column_int(st,3);
                int disp = apply_margin_cents(base);
                if (!first) out << ",";
                first=false;
                out << "{"
                    << "\"id\":" << id << ","
                    << "\"name\":\"" << json_escape(name) << "\","
                    << "\"image_url\":\"" << json_escape(img) << "\","
                    << "\"display_price_cents\":" << disp
                    << "}";
            }
            sqlite3_finalize(st);
        }
        out << "]";
        send_response(cfd, 200, "OK", "application/json; charset=utf-8", out.str());
        return;
    }

    // Not found
    send_response(cfd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found");
}

// ---------------- Main -----------------
int run_server(const Config& cfg) {
    if (!g_db.open(cfg.db_path)) {
        std::cerr << "DB open failed\n"; return 1;
    }
    std::string err;
    if (!g_db.init_schema(&err)) {
        std::cerr << "Schema init failed: " << err << "\n"; return 1;
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }
    int opt=1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{}; addr.sin_family=AF_INET;
    addr.sin_port = htons(cfg.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sfd); return 1; }
    if (listen(sfd, 16) < 0) { perror("listen"); close(sfd); return 1; }

    std::cout << "🚀 RangoonsCore e-commerce starting...\n";
    std::cout << "🌐 Listening on 0.0.0.0:" << cfg.port << "\n";

    for (;;) {
        sockaddr_in cli{}; socklen_t cl=sizeof(cli);
        int cfd = accept(sfd, (sockaddr*)&cli, &cl);
        if (cfd < 0) continue;

        std::string raw;
        if (!recv_all(cfd, raw)) { close(cfd); continue; }

        HttpRequest req;
        if (!parse_request(raw, req)) {
            send_response(cfd, 400, "Bad Request", "text/plain; charset=utf-8", "Bad Request");
            close(cfd); continue;
        }

        handle_request(cfd, req);
        close(cfd);
    }
    return 0;
}
