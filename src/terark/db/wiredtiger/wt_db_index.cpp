#include "wt_db_index.hpp"
#include "wt_db_context.hpp"
#include <terark/io/FileStream.hpp>
#include <terark/io/StreamBuffer.hpp>
#include <terark/io/DataIO.hpp>
#include <terark/lcast.hpp>
#include <terark/util/sortable_strvec.hpp>
#include <boost/filesystem.hpp>

namespace terark { namespace db { namespace wt {

namespace fs = boost::filesystem;

class WtWritableIndex::MyIndexIterBase : public IndexIterator {
protected:
	typedef boost::intrusive_ptr<WtWritableIndex> MockWritableIndexPtr;
	WtWritableIndexPtr m_index;
	WT_CURSOR* m_iter;
	valvec<byte> m_buf;
	MyIndexIterBase(const WtWritableIndex* owner) {
		m_isUniqueInSchema = owner->m_schema->m_isUnique;
		m_index.reset(const_cast<WtWritableIndex*>(owner));
		WT_CONNECTION* conn = m_index->m_wtSession->connection;
		WT_SESSION* session; // WT_SESSION is not thread safe
		int err = conn->open_session(conn, NULL, NULL, &session);
		if (err) {
			THROW_STD(invalid_argument
				, "FATAL: wiredtiger open session(dir=%s) = %s"
				, conn->get_home(conn), wiredtiger_strerror(err)
				);
		}
		const std::string& uri = m_index->m_uri;
		err = session->open_cursor(session, uri.c_str(), NULL, NULL, &m_iter);
		if (err) {
			session->close(session, NULL);
			THROW_STD(logic_error, "open_cursor failed: %s", wiredtiger_strerror(err));
		}
	}
	~MyIndexIterBase() {
		WT_SESSION* session = m_iter->session;
		m_iter->close(m_iter);
		session->close(session, NULL);
	}
	void reset() override {
		m_iter->reset(m_iter);
	}
};

class WtWritableIndex::MyIndexIterForward : public MyIndexIterBase {
public:
	MyIndexIterForward(const WtWritableIndex* o) : MyIndexIterBase(o) {}
	bool increment(llong* id, valvec<byte>* key) override {
		assert(nullptr != m_iter);
		int err = m_iter->next(m_iter);
		if (0 == err) {
			m_index->getKeyVal(m_iter, key, id);
			return true;
		}
		if (WT_NOTFOUND != err) {
			THROW_STD(logic_error, "cursor_next failed: %s", wiredtiger_strerror(err));
		}
		return false;
	}
	int seekLowerBound(fstring key, llong* id, valvec<byte>* retKey) override {
		int cmp = 0;
		WT_ITEM item;
		m_index->setKeyVal(m_iter, key, 0, &item, &m_buf);
		int err = m_iter->search_near(m_iter, &cmp);
		if (err == 0) {
			if (cmp < 0) {
				return -1;
			}
			m_index->getKeyVal(m_iter, retKey, id);
			return key == fstring(*retKey) ? 0 : 1;
		}
		if (WT_NOTFOUND == err) {
			return -1;
		}
		THROW_STD(logic_error, "cursor_search_near failed: %s", wiredtiger_strerror(err));
	}
};

class WtWritableIndex::MyIndexIterBackward : public MyIndexIterBase {
public:
	MyIndexIterBackward(const WtWritableIndex* o) : MyIndexIterBase(o) {}
	bool increment(llong* id, valvec<byte>* key) override {
		assert(nullptr != m_iter);
		int err = m_iter->prev(m_iter);
		if (0 == err) {
			m_index->getKeyVal(m_iter, key, id);
			return true;
		}
		if (WT_NOTFOUND != err) {
			THROW_STD(logic_error, "cursor_next failed: %s", wiredtiger_strerror(err));
		}
		return false;
	}
	int seekLowerBound(fstring key, llong* id, valvec<byte>* retKey) override {
		int cmp = 0;
		WT_ITEM item;
		m_index->setKeyVal(m_iter, key, 0, &item, &m_buf);
		int err = m_iter->search_near(m_iter, &cmp);
		if (err == 0) {
			if (cmp > 0) {
				return increment(id, retKey) ? 1 : -1;
			}
			m_index->getKeyVal(m_iter, retKey, id);
			return key == fstring(*retKey) ? 0 : 1;
		}
		if (WT_NOTFOUND == err) {
			return -1;
		}
		THROW_STD(logic_error, "cursor_search_near failed: %s", wiredtiger_strerror(err));
	}
};

static
std::string toWtSchema(const Schema& schema) {
#if 0
	std::string fmt = "key_format=";
	for (size_t i = 0; i < schema.m_columnsMeta.end_i(); ++i) {
		const ColumnMeta& colmeta = schema.m_columnsMeta.val(i);
		switch (colmeta.type) {
		default:
			THROW_STD(invalid_argument
				, "coltype=%s is not supported by wiredtiger"
				, schema.columnTypeStr(colmeta.type)
				);
		case ColumnType::Binary:
		case ColumnType::CarBin: fmt += 'u'; break;
		case ColumnType::Uint08: fmt += 'B'; break;
		case ColumnType::Sint08: fmt += 'b'; break;
		case ColumnType::Uint16: fmt += 'H'; break;
		case ColumnType::Sint16: fmt += 'h'; break;
		case ColumnType::Uint32: fmt += 'I'; break;
		case ColumnType::Sint32: fmt += 'i'; break;
		case ColumnType::Uint64: fmt += 'Q'; break;
		case ColumnType::Sint64: fmt += 'q'; break;
	//	case ColumnType::Uint128: break;
	//	case ColumnType::Sint128: break;
		case ColumnType::Float32: fmt += 'q'; break;
		case ColumnType::Float64: fmt += 'q'; break;
		case ColumnType::Float128:
		case ColumnType::Uuid:    fmt += "16s"; break;// 16 bytes(128 bits) binary
		case ColumnType::Fixed:
			fmt += lcast(colmeta.fixedLen);
			fmt += 's';
			break;
	//	case ColumnType::VarSint:
	//	case ColumnType::VarUint:
		case ColumnType::StrZero: fmt += 'S'; break;
		case ColumnType::TwoStrZero: fmt += "SS"; break;
		}
	}
	fmt += ",value_format=r";
	return fmt;
#else
	if (schema.m_isUnique) {
	//	if (schema.getFixedRowLen())
	//		return "key_format=" + lcast(schema.getFixedRowLen()) + "s,value_format=q";
	//	else
			return "key_format=u,value_format=q";
	}
	else {
	//	if (schema.getFixedRowLen())
	//		return "key_format=" + lcast(schema.getFixedRowLen()) + "sq,value_format=1t";
	//	else
			return "key_format=uq,value_format=1t";
	}
#endif
}

void
WtWritableIndex::getKeyVal(WT_CURSOR* cursor, valvec<byte>* key, llong* recId)
const {
	getKeyVal(*m_schema, cursor, key, recId);
}

void WtWritableIndex::getKeyVal(const Schema& schema, WT_CURSOR* cursor,
								valvec<byte>* key, llong* recId)
{
	WT_ITEM item;
	memset(&item, 0, sizeof(item));
	if (schema.m_isUnique) {
		cursor->get_key(cursor, &item);
		cursor->get_value(cursor, recId);
	}
	else {
		cursor->get_key(cursor, &item, recId);
	//	cursor->get_value(cursor, ...); // has no value
	}
	key->assign((const byte*)item.data, item.size);
	if (schema.m_needEncodeToLexByteComparable) {
		schema.byteLexConvert(key->data(), item.size);
	}
}

void WtWritableIndex::setKeyVal(WT_CURSOR* cursor, fstring key, llong recId,
								WT_ITEM* item, valvec<byte>* buf)
const {
	setKeyVal(*m_schema, cursor, key, recId, item, buf);
}
void WtWritableIndex::setKeyVal(const Schema& schema, WT_CURSOR* cursor,
								fstring key, llong recId,
								WT_ITEM* item, valvec<byte>* buf)
{
	memset(item, 0, sizeof(*item));
	item->size = key.size();
	if (schema.m_needEncodeToLexByteComparable) {
		buf->assign(key);
		schema.byteLexConvert(*buf);
		item->data = buf->data();
	}
	else {
		item->data = key.data();
	}
	if (schema.m_isUnique) {
		cursor->set_key(cursor, item);
		cursor->set_value(cursor, recId);
	}
	else {
		cursor->set_key(cursor, item, recId);
		cursor->set_value(cursor, 1);
	}
}

WtWritableIndex::WtWritableIndex(const Schema& schema, WT_CONNECTION* conn) {
	WT_SESSION* session;
	int err = conn->open_session(conn, NULL, NULL, &session);
	if (err) {
		THROW_STD(invalid_argument, "FATAL: wiredtiger open session(dir=%s) = %s"
			, conn->get_home(conn), wiredtiger_strerror(err)
			);
	}
	m_keyFmt = toWtSchema(schema);
	m_uri = "table:" + schema.m_name;
	std::replace(m_uri.begin(), m_uri.end(), ',', '.');
	err = session->create(session, m_uri.c_str(), m_keyFmt.c_str());
	if (err) {
		session->close(session, NULL);
		THROW_STD(invalid_argument, "FATAL: wiredtiger create(%s, dir=%s) = %s"
			, m_keyFmt.c_str()
			, conn->get_home(conn), wiredtiger_strerror(err)
			);
	}
	err = session->open_cursor(session,
			m_uri.c_str(), NULL, "overwrite=true", &m_wtReplace);
	if (err) {
		session->close(session, NULL);
		THROW_STD(logic_error, "open_cursor failed: %s", wiredtiger_strerror(err));
	}
	err = session->open_cursor(session,
			m_uri.c_str(), NULL, "overwrite=false", &m_wtCursor);
	if (err) {
		m_wtReplace->close(m_wtReplace);
		session->close(session, NULL);
		THROW_STD(logic_error, "open_cursor failed: %s", wiredtiger_strerror(err));
	}
	this->m_wtSession = session;
	this->m_isUnique = schema.m_isUnique;
	this->m_schema = &schema;
}

WtWritableIndex::~WtWritableIndex() {
	m_wtCursor->close(m_wtCursor);
	m_wtReplace->close(m_wtReplace);
	m_wtSession->close(m_wtSession, NULL);
}

IndexIterator* WtWritableIndex::createIndexIterForward(DbContext*) const {
	tbb::mutex::scoped_lock lock(m_wtMutex);
	return new MyIndexIterForward(this);
}

IndexIterator* WtWritableIndex::createIndexIterBackward(DbContext*) const {
	tbb::mutex::scoped_lock lock(m_wtMutex);
	return new MyIndexIterBackward(this);
}

void WtWritableIndex::save(PathRef path1) const {
#if 0
	int err = m_wtSession->checkpoint(m_wtSession, nullptr);
	if (err != 0) {
		THROW_STD(logic_error, "wt_checkpoint failed: %s", wiredtiger_strerror(err));
	}
#endif
}

void WtWritableIndex::load(PathRef path1) {
	WT_CONNECTION* conn = m_wtSession->connection;
	boost::filesystem::path segDir = conn->get_home(conn);
	auto fpath = segDir / m_uri.substr(6); // remove beginning "table:"
	m_indexStorageSize = boost::filesystem::file_size(fpath);
}

llong WtWritableIndex::indexStorageSize() const {
	return m_indexStorageSize;
}

bool WtWritableIndex::insert(fstring key, llong id, DbContext* ctx) {
	tbb::mutex::scoped_lock lock(m_wtMutex);
	WT_ITEM item;
	setKeyVal(m_wtCursor, key, id, &item, &ctx->buf1);
	int err = m_wtCursor->insert(m_wtCursor);
	if (err == WT_DUPLICATE_KEY) {
	//	fprintf(stderr, "wiredtiger dupkey: %s\n", m_schema->toJsonStr(key).c_str());
		return false;
	}
	if (err) {
		THROW_STD(invalid_argument
			, "FATAL: wiredtiger insert(dir=%s, uri=%s, key=%s) = %s"
			, m_wtSession->connection->get_home(m_wtSession->connection)
			, m_uri.c_str(), m_schema->toJsonStr(key).c_str()
			, wiredtiger_strerror(err)
			);
	}
	m_indexStorageSize += key.size() + sizeof(id); // estimate
	return true;
}

bool WtWritableIndex::replace(fstring key, llong oldId, llong newId, DbContext* ctx) {
	tbb::mutex::scoped_lock lock(m_wtMutex);
	WT_CURSOR* cursor = m_wtReplace;
	WT_ITEM item;
	memset(&item, 0, sizeof(item));
	item.size = key.size();
	if (m_schema->m_needEncodeToLexByteComparable) {
		ctx->buf1.assign(key);
		m_schema->byteLexConvert(ctx->buf1);
		item.data = ctx->buf1.data();
	}
	else {
		item.data = key.data();
	}
	if (m_isUnique) {
		cursor->set_key(cursor, &item);
		cursor->set_value(cursor, newId);
	}
	else {
		cursor->set_key(cursor, &item, oldId);
		cursor->remove(cursor);
		cursor->set_key(cursor, &item, newId);
		cursor->set_value(m_wtCursor, 1);
	}
	int err = cursor->insert(cursor);
	if (err == WT_DUPLICATE_KEY) {
		return false;
	}
	if (err) {
		THROW_STD(invalid_argument, "FATAL: wiredtiger replace(dir=%s, uri=%s, key=%s) = %s"
			, m_wtSession->connection->get_home(m_wtSession->connection)
			, m_uri.c_str(), m_schema->toJsonStr(key).c_str()
			, wiredtiger_strerror(err)
			);
	}
// estimate size don't change
//	m_indexStorageSize += key.size() + sizeof(id); // estimate
	return true;
}

void
WtWritableIndex::searchExactAppend(fstring key, valvec<llong>* recIdvec, DbContext* ctx)
const {
	THROW_STD(invalid_argument, "This method should not be called");
	tbb::mutex::scoped_lock lock(m_wtMutex);
	WT_ITEM item;
	memset(&item, 0, sizeof(item));
	item.size = key.size();
	if (m_schema->m_needEncodeToLexByteComparable) {
		ctx->buf1.assign(key);
		m_schema->byteLexConvert(ctx->buf1);
		item.data = ctx->buf1.data();
	}
	else {
		item.data = key.data();
	}
	if (m_isUnique) {
		m_wtCursor->set_key(m_wtCursor, &item);
		int err = m_wtCursor->search(m_wtCursor);
		if (err == WT_NOTFOUND) {
			return;
		}
		if (err) {
			THROW_STD(invalid_argument
				, "FATAL: wiredtiger search(dir=%s, uri=%s, key=%s) = %s"
				, m_wtSession->connection->get_home(m_wtSession->connection)
				, m_uri.c_str(), m_schema->toJsonStr(key).c_str()
				, wiredtiger_strerror(err)
				);
		}
		llong id = -1;
		m_wtCursor->get_value(m_wtCursor, &id);
		recIdvec->push_back(id);
	}
	else {
		m_wtCursor->set_key(m_wtCursor, &item, llong(-1));
		int cmp;
		int err = m_wtCursor->search_near(m_wtCursor, &cmp);
		if (WT_NOTFOUND == err) {
			return;
		}
		if (err) {
			THROW_STD(invalid_argument
				, "FATAL: wiredtiger search_near(dir=%s, uri=%s, key=%s) = %s"
				, m_wtSession->connection->get_home(m_wtSession->connection)
				, m_uri.c_str(), m_schema->toJsonStr(key).c_str()
				, wiredtiger_strerror(err)
				);
		}
		if (cmp >= 0) {
			WT_ITEM item2;
			while (0 == err) {
				llong id;
				m_wtCursor->get_key(m_wtCursor, &item2, &id);
				if (item2.size == item.size &&
					memcmp(item2.data, item.data, item.size) == 0)
				{
					recIdvec->push_back(id);
					err = m_wtCursor->next(m_wtCursor);
				}
				else break;
			}
		}
	}
	m_wtCursor->reset(m_wtCursor);
}

bool WtWritableIndex::remove(fstring key, llong id, DbContext* ctx) {
	tbb::mutex::scoped_lock lock(m_wtMutex);
	WT_ITEM item;
	memset(&item, 0, sizeof(item));
	item.size = key.size();
	if (m_schema->m_needEncodeToLexByteComparable) {
		ctx->buf1.assign(key);
		m_schema->byteLexConvert(ctx->buf1);
		item.data = ctx->buf1.data();
	}
	else {
		item.data = key.data();
	}
	m_wtCursor->set_key(m_wtCursor, &item);
	int err = m_wtCursor->remove(m_wtCursor);
	if (WT_NOTFOUND == err) {
		fprintf(stderr
			, "WARN: wt_remove non-existing key = %s\n"
			, m_schema->toJsonStr(key).c_str()
			);
		return false;
	}
	if (err) {
		THROW_STD(logic_error, "remove failed: %s", wiredtiger_strerror(err));
	}
	m_wtCursor->reset(m_wtCursor);
	m_indexStorageSize -= key.size() + sizeof(id); // estimate
	return true;
}

void WtWritableIndex::clear() {
	tbb::mutex::scoped_lock lock(m_wtMutex);
	int err = m_wtSession->truncate(m_wtSession, m_uri.c_str(), NULL, NULL, NULL);
	if (err != 0) {
		THROW_STD(logic_error, "truncate failed: %s", wiredtiger_strerror(err));
	}
	m_indexStorageSize = 0;
}

}}} // namespace terark::db::wt
