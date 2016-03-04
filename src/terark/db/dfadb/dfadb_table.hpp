#pragma once

#include <terark/db/db_table.hpp>
#include <nark/fsa/fsa.hpp>
#include <nark/int_vector.hpp>
#include <nark/rank_select.hpp>
#include <nark/fsa/nest_trie_dawg.hpp>

namespace terark {
//	class Nest
} // namespace terark

namespace terark { namespace db { namespace dfadb {

class NARK_DB_DLL DfaDbContext : public DbContext {
public:
	explicit DfaDbContext(const CompositeTable* tab);
	~DfaDbContext();
	std::string m_nltRecBuf;
};
class NARK_DB_DLL DfaDbTable : public CompositeTable {
public:
	DbContext* createDbContext() const override;
	ReadonlySegment* createReadonlySegment(PathRef dir) const override;
	WritableSegment* createWritableSegment(PathRef dir) const override;
	WritableSegment* openWritableSegment(PathRef dir) const override;
};

}}} // namespace terark::db::dfadb