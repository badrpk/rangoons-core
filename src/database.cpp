#include "rangoons.h"
#include <sqlite3.h>
#include <sstream>
#include <iostream>

static int exec_sql(sqlite3* db, const char* sql, std::string* err=nullptr) {
    char* e=nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &e);
    if (rc!=SQLITE_OK) {
        if (err && e) *err = e;
        sqlite3_free(e);
    }
    return rc;
}

bool DB::open(const std::string& path) {
    close();
    return sqlite3_open(path.c_str(), (sqlite3**)&handle) == SQLITE_OK;
}
void DB::close() {
    if (handle) { sqlite3_close((sqlite3*)handle); handle=nullptr; }
}

bool DB::init_schema(std::string* err) {
    const char* sql =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS products ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  description TEXT DEFAULT '',"
        "  price_cents INTEGER NOT NULL,"
        "  stock INTEGER NOT NULL DEFAULT 0,"
        "  image_url TEXT DEFAULT '',"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS carts ("
        "  cart_id TEXT PRIMARY KEY,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS cart_items ("
        "  cart_id TEXT NOT NULL,"
        "  product_id INTEGER NOT NULL,"
        "  qty INTEGER NOT NULL,"
        "  PRIMARY KEY (cart_id, product_id),"
        "  FOREIGN KEY(product_id) REFERENCES products(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS orders ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cart_id TEXT NOT NULL,"
        "  customer_name TEXT NOT NULL,"
        "  phone TEXT NOT NULL,"
        "  address TEXT NOT NULL,"
        "  total_cents INTEGER NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";
    return exec_sql((sqlite3*)handle, sql, err) == SQLITE_OK;
}

bool DB::create_product(const Product& p, int* out_id, std::string* err) {
    const char* sql = "INSERT INTO products(name,description,price_cents,stock,image_url) VALUES(?,?,?,?,?);";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, p.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, p.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, p.price_cents);
    sqlite3_bind_int(st, 4, p.stock);
    sqlite3_bind_text(st, 5, p.image_url.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st)==SQLITE_DONE;
    if (ok && out_id) *out_id = (int)sqlite3_last_insert_rowid((sqlite3*)handle);
    sqlite3_finalize(st);
    return ok;
}

std::vector<Product> DB::list_products() {
    std::vector<Product> v;
    const char* sql = "SELECT id,name,description,price_cents,stock,image_url FROM products ORDER BY created_at DESC;";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return v;
    while (sqlite3_step(st)==SQLITE_ROW) {
        Product p;
        p.id = sqlite3_column_int(st,0);
        p.name = (const char*)sqlite3_column_text(st,1);
        p.description = (const char*)sqlite3_column_text(st,2);
        p.price_cents = sqlite3_column_int(st,3);
        p.stock = sqlite3_column_int(st,4);
        p.image_url = (const char*)sqlite3_column_text(st,5);
        v.push_back(std::move(p));
    }
    sqlite3_finalize(st);
    return v;
}

std::optional<Product> DB::get_product(int id) {
    const char* sql = "SELECT id,name,description,price_cents,stock,image_url FROM products WHERE id=?;";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return std::nullopt;
    sqlite3_bind_int(st,1,id);
    Product p;
    if (sqlite3_step(st)==SQLITE_ROW) {
        p.id = sqlite3_column_int(st,0);
        p.name = (const char*)sqlite3_column_text(st,1);
        p.description = (const char*)sqlite3_column_text(st,2);
        p.price_cents = sqlite3_column_int(st,3);
        p.stock = sqlite3_column_int(st,4);
        p.image_url = (const char*)sqlite3_column_text(st,5);
        sqlite3_finalize(st);
        return p;
    }
    sqlite3_finalize(st);
    return std::nullopt;
}

bool DB::delete_product(int id, std::string* err) {
    const char* sql = "DELETE FROM products WHERE id=?;";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return false;
    sqlite3_bind_int(st,1,id);
    bool ok = sqlite3_step(st)==SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool DB::ensure_cart(const std::string& cart_id) {
    const char* sql = "INSERT OR IGNORE INTO carts(cart_id) VALUES(?);";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return false;
    sqlite3_bind_text(st,1,cart_id.c_str(),-1,SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st)==SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool DB::add_to_cart(const std::string& cart_id, int product_id, int qty, std::string* err) {
    const char* sql =
        "INSERT INTO cart_items(cart_id,product_id,qty) VALUES(?,?,?) "
        "ON CONFLICT(cart_id,product_id) DO UPDATE SET qty=qty+excluded.qty;";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return false;
    sqlite3_bind_text(st,1,cart_id.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(st,2,product_id);
    sqlite3_bind_int(st,3,qty);
    bool ok = sqlite3_step(st)==SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

std::vector<CartItem> DB::get_cart_items(const std::string& cart_id) {
    std::vector<CartItem> v;
    const char* sql =
        "SELECT ci.product_id, ci.qty, p.name, p.price_cents "
        "FROM cart_items ci JOIN products p ON p.id=ci.product_id "
        "WHERE ci.cart_id=?;";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return v;
    sqlite3_bind_text(st,1,cart_id.c_str(),-1,SQLITE_TRANSIENT);
    while (sqlite3_step(st)==SQLITE_ROW) {
        CartItem it;
        it.product_id = sqlite3_column_int(st,0);
        it.qty = sqlite3_column_int(st,1);
        it.name = (const char*)sqlite3_column_text(st,2);
        it.price_cents = sqlite3_column_int(st,3);
        v.push_back(std::move(it));
    }
    sqlite3_finalize(st);
    return v;
}

bool DB::clear_cart(const std::string& cart_id) {
    const char* sql = "DELETE FROM cart_items WHERE cart_id=?;";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return false;
    sqlite3_bind_text(st,1,cart_id.c_str(),-1,SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st)==SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool DB::create_order(const Order& o, int* out_id, std::string* err) {
    const char* sql = "INSERT INTO orders(cart_id,customer_name,phone,address,total_cents) VALUES(?,?,?,?,?);";
    sqlite3_stmt* st=nullptr;
    if (sqlite3_prepare_v2((sqlite3*)handle, sql, -1, &st, nullptr)!=SQLITE_OK) return false;
    sqlite3_bind_text(st,1,o.cart_id.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,2,o.customer_name.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,3,o.phone.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,4,o.address.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(st,5,o.total_cents);
    bool ok = sqlite3_step(st)==SQLITE_DONE;
    if (ok && out_id) *out_id = (int)sqlite3_last_insert_rowid((sqlite3*)handle);
    sqlite3_finalize(st);
    return ok;
}
