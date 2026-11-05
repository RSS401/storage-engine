#pragma once
#include <rohandb/common/serialization.hpp>
#include <rohandb/disk_manager.hpp>
#include <rohandb/storage/database_identity.hpp>
#include <rohandb/storage/page_layout.hpp>
namespace rohandb {
class LogManager;
class OperationContext;
// Exclusive owner of page zero. Never cached or evicted by the buffer pool.
class DatabaseMetaManager {
  public:
    explicit DatabaseMetaManager(IDiskManager &disk) : disk_(disk) {}
    void setLog(LogManager *log) { log_ = log; }
    void initialiseNew();
    void publishInitial(ConstPageView root);
    DatabaseId databaseId() const { return readId(bytes_, layout::DATABASE_ID); }
    void load();
    PageId allocatePageId();
    PageId rootPageId() const { return readScalar<PageId>(bytes_, layout::ROOT); }
    PageId nextPageId() const { return readScalar<PageId>(bytes_, layout::NEXT_PAGE); }
    void setRootPageId(PageId id);
    bool isDirty() const { return dirty_; }
    void writeIfDirty();
    PageView bytes() { return bytes_; }
    ConstPageView bytes() const { return bytes_; }

  private:
    friend class OperationContext;
    LogManager *log_ = nullptr;
    IDiskManager &disk_;
    PageBytes bytes_{};
    bool dirty_ = false;
};
} // namespace rohandb
