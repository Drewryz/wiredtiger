
/*��session��cache read generation���Լ�*/
static inline void __wt_cache_read_gen_incr(WT_SESSION_IMPL* session)
{
	++S2C(session)->cache->read_gen;
}



