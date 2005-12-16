
#include "include/types.h"

#include "OSD.h"
#include "OSDMap.h"

#ifdef USE_OBFS
# include "OBFSStore.h"
#else
# include "FakeStore.h"
#endif

#ifdef USE_EBOFS
# include "ebofs/Ebofs.h"
# include "ebofs/BlockDevice.h"
#endif


#include "mds/MDS.h"

#include "msg/Messenger.h"
#include "msg/Message.h"

#include "msg/HostMonitor.h"

#include "messages/MGenericMessage.h"
#include "messages/MPing.h"
#include "messages/MPingAck.h"
#include "messages/MOSDOp.h"
#include "messages/MOSDOpReply.h"

#include "messages/MOSDMap.h"
#include "messages/MOSDPGNotify.h"
#include "messages/MOSDPGPeer.h"
#include "messages/MOSDPGPeerAck.h"
#include "messages/MOSDPGUpdate.h"

#include "common/Logger.h"
#include "common/LogType.h"
#include "common/Timer.h"
#include "common/ThreadPool.h"

#include <iostream>
#include <cassert>
#include <errno.h>
#include <sys/stat.h>


#include "config.h"
#undef dout
#define  dout(l)    if (l<=g_conf.debug || l<=g_conf.debug_osd) cout << "osd" << whoami << " "

char *osd_base_path = "./osddata";
char *ebofs_base_path = "./ebofsdev";
char ebofs_path[100];

#define ROLE_TYPE(x)   ((x)>0 ? 1:(x))



// cons/des

LogType osd_logtype;


OSD::OSD(int id, Messenger *m) 
{
  whoami = id;

  messenger = m;

  osdmap = 0;

  last_tid = 0;

  max_recovery_ops = 5;

  pending_ops = 0;
  waiting_for_no_ops = false;


  // use fake store
#ifdef USE_OBFS
  store = new OBFSStore(whoami, NULL, "/dev/sdb3");
#else
# ifdef USE_EBOFS
  storedev = 0;
  if (g_conf.osd_ebofs) {
	sprintf(ebofs_path, "%s/%d", ebofs_base_path, whoami);
	storedev = new BlockDevice(ebofs_path);
    store = new Ebofs(*storedev);
  } else 
# endif
	store = new FakeStore(osd_base_path, whoami);
  
#endif

  // monitor
  char s[80];
  sprintf(s, "osd%d", whoami);
  string st = s;
  monitor = new HostMonitor(m, st);
  monitor->set_notify_port(MDS_PORT_OSDMON);
  
  // hack
  int i = whoami;
  if (++i == g_conf.num_osd) i = 0;
  monitor->get_hosts().insert(MSG_ADDR_OSD(i));
  if (++i == g_conf.num_osd) i = 0;
  monitor->get_hosts().insert(MSG_ADDR_OSD(i));
  if (++i == g_conf.num_osd) i = 0;  
  monitor->get_hosts().insert(MSG_ADDR_OSD(i));
  
  monitor->get_notify().insert(MSG_ADDR_MDS(0));

  // log
  char name[80];
  sprintf(name, "osd%02d", whoami);
  logger = new Logger(name, (LogType*)&osd_logtype);
  osd_logtype.add_inc("op");
  osd_logtype.add_inc("rd");
  osd_logtype.add_inc("rdb");
  osd_logtype.add_inc("wr");
  osd_logtype.add_inc("wrb");

  // Thread pool
  {
	char name[80];
	sprintf(name,"osd%d.threadpool", whoami);
	threadpool = new ThreadPool<OSD, MOSDOp>(name, g_conf.osd_maxthreads, (void (*)(OSD*, MOSDOp*))doop, this);
  }
}

OSD::~OSD()
{
  if (threadpool) { delete threadpool; threadpool = 0; }
  if (osdmap) { delete osdmap; osdmap = 0; }
  if (monitor) { delete monitor; monitor = 0; }
  if (messenger) { delete messenger; messenger = 0; }
  if (logger) { delete logger; logger = 0; }
  if (store) { delete store; store = 0; }
#ifdef USE_EBOFS
  if (storedev) { delete storedev; storedev = 0; }
#endif

}

int OSD::init()
{
  osd_lock.Lock();

#ifdef USE_EBOFS
  if (storedev) 
	storedev->open();
#endif
  if (g_conf.osd_mkfs) store->mkfs();
  int r = store->mount();

  monitor->init();

  osd_lock.Unlock();

  // i'm ready!
  messenger->set_dispatcher(this);

  return r;
}

int OSD::shutdown()
{
  dout(1) << "shutdown" << endl;

  // stop threads
  delete threadpool;
  threadpool = 0;

  // shut everything else down
  monitor->shutdown();
  messenger->shutdown();

  int r = store->umount();
#ifdef USE_EBOFS
  if (storedev) storedev->close();
#endif
  return r;
}



// object locks

void OSD::lock_object(object_t oid) 
{
  osd_lock.Lock();
  if (object_lock.count(oid)) {
	Cond c;
	dout(15) << "lock_object " << hex << oid << dec << " waiting as " << &c << endl;
	object_lock_waiters[oid].push_back(&c);
	c.Wait(osd_lock);
	assert(object_lock.count(oid));
  } else {
	dout(15) << "lock_object " << hex << oid << dec << endl;
	object_lock.insert(oid);
  }
  osd_lock.Unlock();
}

void OSD::unlock_object(object_t oid) 
{
  osd_lock.Lock();
  assert(object_lock.count(oid));
  if (object_lock_waiters.count(oid)) {
	// someone is in line
	list<Cond*>& ls = object_lock_waiters[oid];
	Cond *c = ls.front();
	dout(15) << "unlock_object " << hex << oid << dec << " waking up next guy " << c << endl;
	ls.pop_front();
	if (ls.empty()) 
	  object_lock_waiters.erase(oid);
	c->Signal();
  } else {
	// nobody waiting
	dout(15) << "unlock_object " << hex << oid << dec << endl;
	object_lock.erase(oid);
  }
  osd_lock.Unlock();
}



// --------------------------------------
// dispatch

void OSD::dispatch(Message *m) 
{
  // check clock regularly
  g_clock.now();


  switch (m->get_type()) {

	// -- don't need OSDMap --

	// host monitor
  case MSG_PING_ACK:
  case MSG_FAILURE_ACK:
	monitor->proc_message(m);
	break;

	// map and replication
  case MSG_OSD_MAP:
	handle_osd_map((MOSDMap*)m);
	break;

	// osd
  case MSG_SHUTDOWN:
	shutdown();
	delete m;
	break;
	
  case MSG_PING:
	// take note.
	monitor->host_is_alive(m->get_source());
	handle_ping((MPing*)m);
	break;
	

	// -- need OSDMap --

  default:
	{
	  // no map?  starting up?
	  if (!osdmap) {
		osd_lock.Lock();
		dout(7) << "no OSDMap, asking MDS" << endl;
		if (waiting_for_osdmap.empty()) 
		  messenger->send_message(new MGenericMessage(MSG_OSD_GETMAP), 
								  MSG_ADDR_MDS(0), MDS_PORT_MAIN);
		waiting_for_osdmap.push_back(m);
		osd_lock.Unlock();
		return;
	  }

	  // need OSDMap
	  switch (m->get_type()) {
		
	  case MSG_OSD_PG_NOTIFY:
		handle_pg_notify((MOSDPGNotify*)m);
		break;
	  case MSG_OSD_PG_PEER:
		handle_pg_peer((MOSDPGPeer*)m);
		break;
	  case MSG_OSD_PG_PEERACK:
		handle_pg_peer_ack((MOSDPGPeerAck*)m);
		break;
	  case MSG_OSD_PG_UPDATE:
		handle_pg_update((MOSDPGUpdate*)m);
		break;

	  case MSG_OSD_OP:
		monitor->host_is_alive(m->get_source());
		handle_op((MOSDOp*)m);
		break;
		
		// for replication etc.
	  case MSG_OSD_OPREPLY:
		monitor->host_is_alive(m->get_source());
		handle_op_reply((MOSDOpReply*)m);
		break;
		
		
	  default:
		dout(1) << " got unknown message " << m->get_type() << endl;
		assert(0);
	  }
	}
  }

  // finishers?
  if (!finished.empty()) {
	list<Message*> waiting;
	waiting.splice(waiting.begin(), finished);
	for (list<Message*>::iterator it = waiting.begin();
		 it != waiting.end();
		 it++) {
	  dispatch(*it);
	}
  }

}


void OSD::handle_op_reply(MOSDOpReply *m)
{
  // did i get a new osdmap?
  if (m->get_map_version() > osdmap->get_version()) {
	dout(3) << "replica op reply includes a new osd map" << endl;
	update_map(m->get_osdmap());
  }

  // handle op
  switch (m->get_op()) {
  case OSD_OP_REP_PULL:
	op_rep_pull_reply(m);
	break;
  case OSD_OP_REP_PUSH:
	op_rep_push_reply(m);
	break;
  case OSD_OP_REP_REMOVE:
	op_rep_remove_reply(m);
	break;

  case OSD_OP_REP_WRITE:
  case OSD_OP_REP_TRUNCATE:
  case OSD_OP_REP_DELETE:
	ack_replica_op(m->get_tid(), m->get_result(), m->get_safe(), MSG_ADDR_NUM(m->get_source()));
	delete m;
	break;

  default:
	assert(0);
  }
}

void OSD::ack_replica_op(__uint64_t tid, int result, bool safe, int fromosd)
{
  //replica_write_lock.Lock();

  if (!replica_ops.count(tid)) {
	dout(7) << "not waiting for tid " << tid << " replica op reply, map must have changed, dropping." << endl;
	return;
  }
  
  
  OSDReplicaOp *repop = replica_ops[tid];
  MOSDOp *op = repop->op;
  pg_t pgid = op->get_pg();

  dout(7) << "ack_replica_op " << tid << " op " << op << " result " << result << " safe " << safe << " from osd" << fromosd << endl;
  dout(15) << " repop was: op " << repop->op << " waitfor ack=" << repop->waitfor_ack << " sync=" << repop->waitfor_sync << " localsync=" << repop->local_sync << " cancel=" << repop->cancel << "  osd=" << repop->osds << endl;


  if (result >= 0) {
	// success
	
	if (safe) {
	  // sync
	  repop->waitfor_sync.erase(tid);
	  repop->waitfor_ack.erase(tid);
	  replica_ops.erase(tid);
	  
	  replica_pg_osd_tids[pgid][fromosd].erase(tid);
	  if (replica_pg_osd_tids[pgid][fromosd].empty()) replica_pg_osd_tids[pgid].erase(fromosd);
	  if (replica_pg_osd_tids[pgid].empty()) replica_pg_osd_tids.erase(pgid);

	  // send 'safe' to client?
	  if (repop->can_send_sync()) {
		if (op->wants_safe()) {
		  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, true);
		  messenger->send_message(reply, op->get_asker());
		}
		delete op;
		delete repop;
	  }
	} else {
	  // ack
	  repop->waitfor_ack.erase(tid);

	  // send 'ack' to client?
	  if (repop->can_send_ack()) {
		if (op->wants_ack()) {
		  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, false);
		  messenger->send_message(reply, op->get_asker());
		}
	  }
	}

  } else {
	// failure
	
	// forget about this failed attempt..
	repop->osds.erase(fromosd);
	repop->waitfor_ack.erase(tid);
	repop->waitfor_sync.erase(tid);

	replica_ops.erase(tid);

	replica_pg_osd_tids[pgid][fromosd].erase(tid);
	if (replica_pg_osd_tids[pgid][fromosd].empty()) replica_pg_osd_tids[pgid].erase(fromosd);
	if (replica_pg_osd_tids[pgid].empty()) replica_pg_osd_tids.erase(pgid);

	bool did = false;
	PG *pg = get_pg(pgid);

	// am i no longer the primary?
	if (pg->get_primary() != whoami) {
	  // oh, it wasn't a replica.. primary must have changed
	  dout(4) << "i'm no longer the primary for " << *pg << endl;

	  // retry the whole thing
	  finished.push_back(repop->op);

	  // clean up
	  for (map<__uint64_t,int>::iterator it = repop->waitfor_ack.begin();
		   it != repop->waitfor_ack.end();
		   it++) {
		replica_ops.erase(it->first);
		replica_pg_osd_tids[pgid][it->second].erase(it->first);
		if (replica_pg_osd_tids[pgid][it->second].empty()) replica_pg_osd_tids[pgid].erase(it->second);
	  }
	  for (map<__uint64_t,int>::iterator it = repop->waitfor_sync.begin();
		   it != repop->waitfor_sync.end();
		   it++) {
		replica_ops.erase(it->first);
		replica_pg_osd_tids[pgid][it->second].erase(it->first);
		if (replica_pg_osd_tids[pgid][it->second].empty()) replica_pg_osd_tids[pgid].erase(it->second);
	  }
	  if (replica_pg_osd_tids[pgid].empty()) replica_pg_osd_tids.erase(pgid);

	  if (repop->local_sync)
		delete repop;
	  else {
		repop->op = 0;      // we're forwarding it
		repop->cancel = true;     // will get deleted by local sync callback
	  }
	  did = true;
	}

	/* no!  don't do this, not without checking complete/clean-ness
	else {
	  // i am still primary.
	  // re-issue replica op to a moved replica?
	  for (unsigned i=1; i<pg->acting.size(); i++) {
		if (repop->osds.count(pg->acting[i])) continue;
		issue_replica_op(pg, repop, pg->acting[i]);
		did = true;
	  }	
	}
	*/

	if (!did) {
	  // an osd musta just gone down or somethin.  are we "done" now?
	  
	  // send 'safe' to client?
	  if (repop->can_send_sync()) {
		if (op->wants_safe()) {
		  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, true);
		  messenger->send_message(reply, op->get_asker());
		}
		delete op;
		delete repop;
	  }
	  
	  // send 'ack' to client?
	  else if (repop->can_send_ack()) {
		if (op->wants_ack()) {
		  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, false);
		  messenger->send_message(reply, op->get_asker());
		}
	  }	  
	}

  }


}



void OSD::handle_ping(MPing *m)
{
  // play dead?
  if (whoami == 1) {
	dout(7) << "playing dead" << endl;
  } else {
	dout(7) << "got ping, replying" << endl;
	messenger->send_message(new MPingAck(m),
							m->get_source(), m->get_source_port(), 0);
  }
  
  delete m;
}



// =====================================================
// MAP

void OSD::wait_for_new_map(Message *m)
{
  // ask MDS
  messenger->send_message(new MGenericMessage(MSG_OSD_GETMAP), 
						  MSG_ADDR_MDS(0), MDS_PORT_MAIN);

  osd_lock.Lock();
  waiting_for_osdmap.push_back(m);
  osd_lock.Unlock();
}


/** update_map
 * assimilate a new OSDMap.  scan pgs.
 */
void OSD::update_map(bufferlist& state, bool mkfs)
{
  // decode new map
  osdmap = new OSDMap();
  osdmap->decode(state);
  osdmaps[osdmap->get_version()] = osdmap;
  dout(7) << "got osd map version " << osdmap->get_version() << endl;
	
  // pg list
  list<pg_t> pg_list;
  
  if (mkfs) {
	// create PGs
	for (int nrep = 2; nrep <= g_conf.osd_max_rep; nrep++) {
	  ps_t maxps = 1LL << osdmap->get_pg_bits();
	  for (pg_t ps = 0; ps < maxps; ps++) {
		pg_t pgid = osdmap->ps_nrep_to_pg(ps, nrep);
		vector<int> acting;
		osdmap->pg_to_acting_osds(pgid, acting);
		
		
		if (acting[0] == whoami) {
		  PG *pg = create_pg(pgid);
		  pg->acting = acting;
		  pg->set_role(0);
		  pg->set_primary_since(osdmap->get_version());
		  pg->mark_complete( osdmap->get_version() );
		  
		  dout(7) << "created " << *pg << endl;
		  
		  pg_list.push_back(pgid);
		}
	  }
	}
  } else {
	// get pg list
	get_pg_list(pg_list);
  }
  
  // use our new map(s)
  advance_map(pg_list);
  activate_map(pg_list);
  
  if (mkfs) {
	// mark all peers complete
	for (list<pg_t>::iterator pgid = pg_list.begin();
		 pgid != pg_list.end();
		 pgid++) {
	  PG *pg = get_pg(*pgid);
	  for (map<int,PGPeer*>::iterator it = pg->peers.begin();
		   it != pg->peers.end();
		   it++) {
		PGPeer *p = it->second;
		//dout(7) << " " << *pg << " telling peer osd" << p->get_peer() << " they are complete" << endl;
		messenger->send_message(new MOSDPGUpdate(osdmap->get_version(), pg->get_pgid(), true, osdmap->get_version()),
								MSG_ADDR_OSD(p->get_peer()));
	  }
	}
  }

  // process waiters
  take_waiters(waiting_for_osdmap);
}

void OSD::handle_osd_map(MOSDMap *m)
{
  // wait for ops to finish
  wait_for_no_ops();

  osd_lock.Lock();     // actually, don't need this if we finish all ops?

  if (m->is_mkfs()) {
	dout(1) << "MKFS" << endl;
	if (!g_conf.osd_mkfs)
	  store->mkfs();
  }

  if (!osdmap ||
	  m->get_version() > osdmap->get_version()) {
	if (osdmap) {
	  dout(3) << "handle_osd_map got osd map version " << m->get_version() << " > " << osdmap->get_version() << endl;
	} else {
	  dout(3) << "handle_osd_map got osd map version " << m->get_version() << endl;
	}

	update_map(m->get_osdmap(), m->is_mkfs());

  } else {
	dout(3) << "handle_osd_map ignoring osd map version " << m->get_version() << " <= " << osdmap->get_version() << endl;
  }
  
  osd_lock.Unlock();

  if (m->is_mkfs()) {
	// ack
	messenger->send_message(new MGenericMessage(MSG_OSD_MKFS_ACK),
							m->get_source());
  }

  delete m;
}


OSDMap* OSD::get_osd_map(version_t v)
{
  assert(osdmaps[v]);
  return osdmaps[v];
}


// ======================================================
// REPLICATION




// ------------------------------------
// placement supersets
/*
void OSD::get_ps_list(list<pg_t>& ls)
{
  list<coll_t>& cl;
  store->list_collections(cl);
  
  for (list<coll_t>::iterator it = cl.begin();
	   it != cl.end();
	   it++) {
	// is it a PS (and not a PG)?
	if (*it & PG_PS_MASK == *it)
	  ls.push_back(*it);
  }
}

bool OSD::ps_exists(ps_t psid)
{
  struct stat st;
  if (store->collection_stat(psid, &st) == 0) 
	return true;
  else
	return false;
}

PS* OSD::create_ps(ps_t psid)
{
  assert(ps_map.count(psid) == 0);
  assert(!ps_exists(psid));

  PS *ps = new PS(psid);
  ps->store(store);
  ps_map[psid] = ps;
  return ps;
}

PS* OSD::open_ps(ps_t psid)
{
  // already open?
  if (ps_map.count(psid)) 
	return ps_map[psid];

  // exists?
  if (!ps_exists(psid))
	return 0;

  // open, stat collection
  PS *ps = new PS(whoami, psid);
  ps->fetch(store);
  ps_map[psid] = ps;

  return ps;
}

void OSD::close_ps(ps_t psid)
{
  assert(0);
}

void OSD::remove_ps(ps_t psid) 
{
  assert(0);
}
*/


// PG

void OSD::get_pg_list(list<pg_t>& ls)
{
  // just list collections; assume they're all pg's (for now)
  store->list_collections(ls);
}

bool OSD::pg_exists(pg_t pgid) 
{
  return store->collection_exists(pgid);
  /*struct stat st;
  if (store->collection_stat(pgid, &st) == 0) 
	return true;
  else
	return false;
  */
}

PG *OSD::create_pg(pg_t pgid)
{
  assert(pg_map.count(pgid) == 0);
  assert(!pg_exists(pgid));

  PG *pg = new PG(whoami, pgid);
  //pg->info.created = osdmap->get_version();
  
  pg->store(store);
  pg_map[pgid] = pg;
  return pg;
}

PG *OSD::get_pg(pg_t pgid)
{
  // already open?
  if (pg_map.count(pgid)) 
	return pg_map[pgid];

  // exists?
  if (!pg_exists(pgid))
	return 0;

  // open, stat collection
  PG *pg = new PG(whoami, pgid);
  pg->fetch(store);
  pg_map[pgid] = pg;

  return pg;
}
 




/** 
 * scan placement groups, initiate any replication
 * activities.
 */
void OSD::advance_map(list<pg_t>& ls)
{
  dout(7) << "advance_map version " << osdmap->get_version() << endl;
  
  // scan pg's
  for (list<pg_t>::iterator it = ls.begin();
	   it != ls.end();
	   it++) {
	pg_t pgid = *it;
	PG *pg = get_pg(pgid);
	assert(pg);
	
	// get new acting set
	vector<int> acting;
	int nrep = osdmap->pg_to_acting_osds(pgid, acting);
	
	int primary = -1;
	if (nrep > 0) primary = acting[0];
	
	int role = -1;        // -1, 0, 1
	for (int i=0; i<nrep; i++) 
	  if (acting[i] == whoami) role = i > 0 ? 1:0;

	// no change?
	if (acting == pg->acting) 
	  continue;
	
	if (role != pg->get_role()) {
	  // role change.
	  dout(10) << " " << *pg << " role change " << pg->get_role() << " -> " << role << endl; 
	  
	  // old primary?
	  if (pg->get_role() == 0) {
		// drop peers
		take_waiters(pg->waiting_for_peered);
		for (hash_map<object_t, list<Message*> >::iterator it = pg->waiting_for_missing_object.begin();
			 it != pg->waiting_for_missing_object.end();
			 it++)
		  take_waiters(it->second);
		pg->waiting_for_missing_object.clear();

		for (hash_map<object_t, list<Message*> >::iterator it = pg->waiting_for_clean_object.begin();
			 it != pg->waiting_for_clean_object.end();
			 it++)
		  take_waiters(it->second);
		pg->waiting_for_clean_object.clear();

		pg->drop_peers();
		pg->state_clear(PG_STATE_CLEAN);
		pg->discard_recovery_plan();
	  }

	  // new primary?
	  if (role == 0) {
		pg->set_primary_since(osdmap->get_version());
		pg->state_clear(PG_STATE_PEERED);
	  } else {
		// we need to announce
		pg->state_set(PG_STATE_STRAY);

		if (nrep == 0) 
		  dout(1) << "crashed pg " << *pg << endl;
	  }
	  
	} else {
	  // no role change.
	  // did primary change?
	  if (primary != pg->get_primary()) {
		dout(10) << " " << *pg << " acting primary change " << pg->get_primary() << " -> " << primary << ", !peered" << endl;
		
		// we need to announce
		pg->state_set(PG_STATE_STRAY);
	  } else {
		// primary is the same.
		if (role == 0) {
		  // i am (still) primary. but replica set changed.
		  dout(10) << " " << *pg << " replica set changed, !clean !peered" << endl;
		  pg->state_clear(PG_STATE_PEERED);
		  pg->state_clear(PG_STATE_CLEAN);
		}
	  }
	}

	// update PG
	pg->acting = acting;
	pg->calc_role(whoami);
	pg->store(store);


	// scan down osds
	for (set<int>::const_iterator down = osdmap->get_down_osds().begin();
		 down != osdmap->get_down_osds().end();
		 down++) {
	  PGPeer *pgp = pg->get_peer(*down);
	  if (!pgp) continue;

	  dout(10) << " " << *pg << " peer osd" << *down << " is down, removing" << endl;
	  pg->remove_peer(*down);
	  
	  // NAK any ops to the down osd
	  if (replica_pg_osd_tids[pgid].count(*down)) {
		set<__uint64_t> s = replica_pg_osd_tids[pgid][*down];
		dout(10) << " " << *pg << " naking replica ops to down osd" << *down << " " << s << endl;
		for (set<__uint64_t>::iterator tid = s.begin();
			 tid != s.end();
			 tid++)
		  ack_replica_op(*tid, -1, false, *down);
	  }
	}

  }

}

void OSD::activate_map(list<pg_t>& ls)
{
  dout(7) << "activate_map version " << osdmap->get_version() << endl;

  map< int, map<pg_t, version_t> > notify_list;  // primary -> pgid -> last_any_complete
  map< int, map<PG*,int> >   start_map;    // peer -> PG -> peer_role

  // scan pg's
  for (list<pg_t>::iterator it = ls.begin();
	   it != ls.end();
	   it++) {
	pg_t pgid = *it;
	PG *pg = get_pg(pgid);
	assert(pg);

	if (pg->get_role() == 0) {
	  // i am primary
	  start_peers(pg, start_map);
	} 
	else if (pg->is_stray()) {
	  // i am residual|replica
	  notify_list[pg->get_primary()][pgid] = pg->get_last_any_complete();
	}
  }  

  // notify? (residual|replica)
  for (map< int, map<pg_t, version_t> >::iterator pit = notify_list.begin();
	   pit != notify_list.end();
	   pit++)
	peer_notify(pit->first, pit->second);

  // start peer? (primary)
  for (map< int, map<PG*, int> >::iterator pit = start_map.begin();
	   pit != start_map.end();
	   pit++)
	peer_start(pit->first, pit->second);

}



/** peer_notify
 * Send an MOSDPGNotify to a primary, with a list of PGs that I have
 * content for, and they are primary for.
 */
void OSD::peer_notify(int primary, map<pg_t, version_t>& pg_list)
{
  dout(7) << "peer_notify osd" << primary << " on " << pg_list.size() << " PGs" << endl;
  MOSDPGNotify *m = new MOSDPGNotify(osdmap->get_version(), pg_list);
  messenger->send_message(m, MSG_ADDR_OSD(primary));
}


void OSD::start_peers(PG *pg, map< int, map<PG*,int> >& start_map) 
{
  dout(10) << " " << *pg << " last_any_complete " << pg->get_last_any_complete() << endl;

  // determine initial peer set
  map<int,int> peerset;  // peer -> role
  
  // prior map(s), if OSDs are still up
  for (version_t epoch = pg->get_last_any_complete();
	   epoch < osdmap->get_version();
	   epoch++) {
	OSDMap *omap = get_osd_map(epoch);
	assert(omap);
	
	vector<int> acting;
	omap->pg_to_acting_osds(pg->get_pgid(), acting);
	
	for (unsigned i=0; i<acting.size(); i++) 
	  if (osdmap->is_up(acting[i]))
		peerset[acting[i]] = -1;
  }
  
  // current map
  for (unsigned i=1; i<pg->acting.size(); i++)
	peerset[pg->acting[i]] = i>0 ? 1:0;
  

  // check peers
  bool havepeers = true;
  for (map<int,int>::iterator it = peerset.begin();
	   it != peerset.end();
	   it++) {
	int who = it->first;
	int role = it->second;
	if (who == whoami) continue;      // nevermind me

	PGPeer *pgp = pg->get_peer(who);
	if (pgp && pgp->is_active() &&
		pgp->get_role() == role) {
	  dout(10) << " " << *pg << " actively peered with osd" << who << " role " << role << endl;	  
	} else {
	  if (pgp) {
		pg->remove_peer(who);
		dout(10) << " " << *pg << " need to re-peer with osd" << who << " role " << role << endl;
	  } else {
		dout(10) << " " << *pg << " need to peer with osd" << who << " role " << role << endl;
	  }
	  start_map[who][pg] = role;
	  havepeers = false;
	}
  }

  if (havepeers && 
	  !pg->is_peered()) {
	dout(10) << " " << *pg << " already has necessary peers, analyzing" << endl;
	pg->mark_peered();
	take_waiters(pg->waiting_for_peered);

	plan_recovery(pg);
	do_recovery(pg);
  }
}


/** peer_start
 * initiate a peer session with a replica on given list of PGs
 */
void OSD::peer_start(int replica, map<PG*,int>& pg_map)
{
  dout(7) << "peer_start with osd" << replica << " on " << pg_map.size() << " PGs" << endl;
  
  list<pg_t> pgids;

  for (map<PG*,int>::iterator it = pg_map.begin();
	   it != pg_map.end();
	   it++) {
	PG *pg = it->first;
	int role = it->second;
	
	assert(pg->get_peer(replica) == 0);
	//PGPeer *p = 
	pg->new_peer(replica, role);
	
	// set last_request stamp?
	// ...

	pgids.push_back(pg->get_pgid());	// add to list
  }

  MOSDPGPeer *m = new MOSDPGPeer(osdmap->get_version(), pgids);
  messenger->send_message(m,
						  MSG_ADDR_OSD(replica));
}



bool OSD::require_current_map(Message *m, version_t v) 
{
  int from = MSG_ADDR_NUM(m->get_source());

  // older map?
  if (v < osdmap->get_version()) {
	dout(7) << "  from old map version " << v << " < " << osdmap->get_version() << endl;
	delete m;   // discard and ignore.
	return false;
  }

  // newer map?
  if (v > osdmap->get_version()) {
	dout(7) << "  from newer map version " << v << " > " << osdmap->get_version() << endl;
	wait_for_new_map(m);
	return false;
  }

  // down?
  if (osdmap->is_down(from)) {
	dout(7) << "  from down OSD osd" << from << ", dropping" << endl;
	// FIXME
	return false;
  }

  assert(v == osdmap->get_version());

  return true;
}


/*
 * require that we have same (or newer) map, and that
 * the source is the pg primary.
 */
bool OSD::require_current_pg_primary(Message *m, version_t v, PG *pg) 
{
  int from = MSG_ADDR_NUM(m->get_source());

  // newer map?
  if (v > osdmap->get_version()) {
	dout(7) << "  from newer map version " << v << " > " << osdmap->get_version() << endl;
	wait_for_new_map(m);
	return false;
  }

  // older map?
  if (v < osdmap->get_version()) {
	// same primary?   
	// FIXME.. line of succession must match!
	if (from != pg->get_primary()) {
	  dout(7) << "  not from pg primary osd" << pg->get_primary() << ", dropping" << endl;
	  delete m;   // discard and ignore.
	  return false;
	}
  }

  // down?
  if (osdmap->is_down(from)) {
	dout(7) << "  from down OSD osd" << from << ", pinging" << endl;
	// FIXME
	return false;
  }

  return true;
}



void OSD::handle_pg_notify(MOSDPGNotify *m)
{
  int from = MSG_ADDR_NUM(m->get_source());
  dout(7) << "handle_pg_notify from osd" << from << endl;

  if (!require_current_map(m, m->get_version())) return;
  
  // look for unknown PGs i'm primary for
  map< int, map<PG*,int> > start_map;

  for (map<pg_t, version_t>::iterator it = m->get_pg_list().begin();
	   it != m->get_pg_list().end();
	   it++) {
	pg_t pgid = it->first;
	PG *pg = get_pg(pgid);

	if (!pg) {
	  pg = create_pg(pgid);

	  int nrep = osdmap->pg_to_acting_osds(pgid, pg->acting);
	  assert(nrep > 0);
	  assert(pg->acting[0] == whoami);
	  pg->set_role(0);
	  pg->set_primary_since( osdmap->get_version() );  // FIXME: this may miss a few epochs!
	  pg->mark_any_complete( it->second );

	  dout(10) << " " << *pg << " is new, nrep=" << nrep << endl;	  

	  // start peers
	  start_peers(pg, start_map);

	  // kick any waiters
	  if (waiting_for_pg.count(pgid)) {
		take_waiters(waiting_for_pg[pgid]);
		waiting_for_pg.erase(pgid);
	  }
	}

	if (pg->is_peered()) {
	  // we're already peered.  what do we do with this guy?
	  assert(0);
	}

	if (it->second > pg->get_last_any_complete())
	  pg->mark_any_complete( it->second );

	// peered with this guy specifically?
	PGPeer *pgp = pg->get_peer(from);
	if (!pgp && 
		start_map[from].count(pg) == 0) {
	  dout(7) << " " << *pg << " primary needs to peer with residual notifier osd" << from << endl;
	  start_map[from][pg] = -1; 
	}
  }
  
  // start peers?
  if (start_map.empty()) {
	dout(7) << " no new peers" << endl;
  } else {
	for (map< int, map<PG*,int> >::iterator pit = start_map.begin();
		 pit != start_map.end();
		 pit++)
	  peer_start(pit->first, pit->second);
  }
  
  delete m;
}

void OSD::handle_pg_peer(MOSDPGPeer *m)
{
  int from = MSG_ADDR_NUM(m->get_source());
  dout(7) << "handle_pg_peer from osd" << from << endl;

  if (!require_current_map(m, m->get_version())) return;

  // go
  MOSDPGPeerAck *ack = new MOSDPGPeerAck(osdmap->get_version());

  for (list<pg_t>::iterator it = m->get_pg_list().begin();
	   it != m->get_pg_list().end();
	   it++) {
	pg_t pgid = *it;
	
	// open PG
	PG *pg = get_pg(pgid);

	// dne?
	if (!pg) {
	  // get active rush mapping
	  vector<int> acting;
	  int nrep = osdmap->pg_to_acting_osds(pgid, acting);
	  assert(nrep > 0);
	  int role = -1;
	  for (unsigned i=0; i<acting.size(); i++)
		if (acting[i] == whoami) role = i>0 ? 1:0;
	  assert(role != 0);

	  if (role < 0) {
		dout(10) << " pg " << hex << pgid << dec << " dne, and i am not an active replica" << endl;
		ack->pg_dne.push_back(pgid);
		continue;
	  }

	  pg = create_pg(pgid);
	  pg->acting = acting;
	  pg->set_role(role);

	  //if (m->get_version() == 1) pg->mark_complete();   // hack... need a more elegant solution

	  dout(10) << " " << *pg << " dne (before), but i am role " << role << endl;

	  // take any waiters
	  if (waiting_for_pg.count(pgid)) {
		take_waiters(waiting_for_pg[pgid]);
		waiting_for_pg.erase(pgid);
	  }
	}

	// PEER

	// report back state and pg content
	ack->pg_state[pgid].state = pg->get_state();
	ack->pg_state[pgid].last_complete = pg->get_last_complete();
	ack->pg_state[pgid].last_any_complete = pg->get_last_any_complete();
	pg->scan_local_objects(ack->pg_state[pgid].objects, store);	// list my objects
	
	// i am now peered
	pg->state_set(PG_STATE_PEERED);
	pg->state_clear(PG_STATE_STRAY);
	
	dout(10) << "sending peer ack " << *pg << " " << ack->pg_state[pgid].objects.size() << " objects" << endl;
  }

  // reply
  messenger->send_message(ack,
						  MSG_ADDR_OSD(from));

  delete m;
}


void OSD::handle_pg_peer_ack(MOSDPGPeerAck *m)
{
  int from = MSG_ADDR_NUM(m->get_source());
  dout(7) << "handle_pg_peer_ack from osd" << from << endl;

  if (!require_current_map(m, m->get_version())) return;

  // pg_dne first
  for (list<pg_t>::iterator it = m->pg_dne.begin();
	   it != m->pg_dne.end();
	   it++) {
	PG *pg = get_pg(*it);
	assert(pg);

	dout(10) << " " << *pg << " dne on osd" << from << endl;
	
	PGPeer *pgp = pg->get_peer(from);
	if (pgp) {
	  pg->remove_peer(from);
	} else {
	  dout(10) << "  weird, i didn't have it!" << endl;   // multiple lagged peer requests?
	  assert(0); // not until peer requests span epochs!
	}
  }

  // pg_state
  for (map<pg_t, PGReplicaInfo>::iterator it = m->pg_state.begin();
	   it != m->pg_state.end();
	   it++) {
	PG *pg = get_pg(it->first);
	assert(pg);

	dout(10) << " " << *pg << " osd" << from << " remote state " << it->second.state 
			 << " w/ " << it->second.objects.size() << " objects"
			 << ", last_complete " << it->second.last_complete 
			 << ", last_any_complete " << it->second.last_any_complete 
			 << endl;

	PGPeer *pgp = pg->get_peer(from);
	assert(pgp);
	
	pg->mark_any_complete( it->second.last_any_complete );

	pgp->last_complete = it->second.last_complete;
	pgp->objects = it->second.objects;
	pgp->state_set(PG_PEER_STATE_ACTIVE);

	// fully peered?
	bool fully = true;
	for (map<int, PGPeer*>::iterator pit = pg->get_peers().begin();
		 pit != pg->get_peers().end();
		 pit++) {
	  dout(10) << " " << *pg << "  peer osd" << pit->first << " state " << pit->second->get_state() << endl;
	  if (!pit->second->is_active()) fully = false;
	}

	if (fully) {
	  if (!pg->is_peered()) {
		// now we're peered!
		pg->mark_peered();
		
		// waiters?
		take_waiters(pg->waiting_for_peered);

		dout(10) << " " << *pg << " fully peered, analyzing" << endl;
		plan_recovery(pg);
		do_recovery(pg);
	  } else {
		// we're already peered.
		// what's the use of this new guy?
		
	  }
	}	  
  }

  // done
  delete m;
}


void OSD::handle_pg_update(MOSDPGUpdate *m)
{
  int from = MSG_ADDR_NUM(m->get_source());
  dout(7) << "handle_pg_update on " << hex << m->get_pgid() << dec << " from osd" << from 
		  << " complete=" << m->is_complete() 
		  << " last_any_complete=" << m->get_last_any_complete()
		  << endl;
  
  PG *pg = get_pg(m->get_pgid());
  if (!require_current_pg_primary(m, m->get_version(), pg)) return;

  // update
  if (!pg) {
	dout(7) << "don't have pg " << hex << m->get_pgid() << dec << endl;
  } else {
	// update my info.   --what info?
	//pg->assim_info( m->get_pginfo() );
	
	// complete?
	if (m->is_complete()) {
	  pg->mark_complete( osdmap->get_version() );
	}

	if (m->get_last_any_complete()) 
	  pg->mark_any_complete( m->get_last_any_complete() );

	pg->store(store);
  }

  delete m;
}



// RECOVERY

void OSD::plan_recovery(PG *pg) 
{
  version_t current_version = osdmap->get_version();
  
  list<PGPeer*> complete_peers;
  pg->plan_recovery(store, current_version, complete_peers);

  for (list<PGPeer*>::iterator it = complete_peers.begin();
	   it != complete_peers.end();
	   it++) {
	PGPeer *p = *it;
	dout(7) << " " << *pg << " telling peer osd" << p->get_peer() << " they are complete" << endl;
	messenger->send_message(new MOSDPGUpdate(osdmap->get_version(), pg->get_pgid(), true, osdmap->get_version()),
							MSG_ADDR_OSD(p->get_peer()));
  }
}

void OSD::do_recovery(PG *pg)
{
  // recover
  if (!pg->is_complete(osdmap->get_version())) {
	pg_pull(pg, max_recovery_ops);
  }
  
  // replicate
  if (pg->is_complete( osdmap->get_version() )) {
	if (!pg->objects_unrep.empty()) 
	  pg_push(pg, max_recovery_ops);
	if (!pg->objects_stray.empty()) 
	  pg_clean(pg, max_recovery_ops);
  }
}


// pull

void OSD::pg_pull(PG *pg, int maxops)
{
  int ops = pg->num_active_ops();

  dout(7) << "pg_pull pg " << hex << pg->get_pgid() << dec << " " << pg->objects_missing.size() << " to do, " << ops << "/" << maxops << " active" <<  endl;
  
  while (ops < maxops) {
	object_t oid;
	if (!pg->get_next_pull(oid)) {
	  dout(7) << "pg_pull done " << *pg << endl;
	  break;
	}
	pull_replica(pg, oid);
	ops++;
  }  
}

void OSD::pull_replica(PG *pg, object_t oid)
{
  version_t v = pg->objects_missing_v[oid];
  
  // choose a peer
  set<int>::iterator pit = pg->objects_missing[oid].begin();
  PGPeer *p = pg->get_peer(*pit);
  dout(7) << "pull_replica " << hex << oid << dec << " v " << v << " from osd" << p->get_peer() << endl;

  // add to fetching list
  pg->pulling(oid, v, p);

  // send op
  __uint64_t tid = ++last_tid;
  MOSDOp *op = new MOSDOp(tid, messenger->get_myaddr(),
						  oid, p->pg->get_pgid(),
						  osdmap->get_version(),
						  OSD_OP_REP_PULL);
  op->set_version(v);
  op->set_pg_role(-1);  // whatever, not 0
  messenger->send_message(op, MSG_ADDR_OSD(p->get_peer()));

  // register
  pull_ops[tid] = p;
}

void OSD::op_rep_pull(MOSDOp *op)
{
  dout(7) << "rep_pull on " << hex << op->get_oid() << dec << " v " << op->get_version() << endl;
  lock_object(op->get_oid());
  
  // get object size
  struct stat st;
  int r = store->stat(op->get_oid(), &st);
  assert(r == 0);

  // check version
  version_t v = 0;
  store->getattr(op->get_oid(), "version", &v, sizeof(v));
  assert(v == op->get_version());
  
  // read
  bufferlist bl;
  long got = store->read(op->get_oid(), 
						 st.st_size, 0,
						 bl);
  assert(got == st.st_size);
  
  // reply
  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, true); 
  reply->set_result(0);
  reply->set_data(bl);
  reply->set_length(got);
  reply->set_offset(0);
  
  messenger->send_message(reply, op->get_asker());

  unlock_object(op->get_oid());
  delete op;
}

void OSD::op_rep_pull_reply(MOSDOpReply *op)
{
  object_t o = op->get_oid();
  version_t v = op->get_version();

  dout(7) << "rep_pull_reply " << hex << o << dec << " v " << v << " size " << op->get_length() << endl;

  osd_lock.Lock();
  PGPeer *p = pull_ops[op->get_tid()];
  PG *pg = p->pg;
  assert(p);   // FIXME: how will this work?
  assert(p->is_pulling(o));
  assert(p->pulling_version(o) == v);
  osd_lock.Unlock();

  // write it and add it to the PG
  store->write(o, op->get_length(), 0, op->get_data());
  p->pg->add_object(store, o);

  store->setattr(o, "version", &v, sizeof(v));

  // close out pull op.
  osd_lock.Lock();
  pull_ops.erase(op->get_tid());

  pg->pulled(o, v, p);

  // now complete?
  if (pg->objects_missing.empty()) {
	pg->mark_complete(osdmap->get_version());

	// distribute new last_any_complete
	dout(7) << " " << *pg << " now complete, updating last_any_complete on peers" << endl;
	for (map<int,PGPeer*>::iterator it = pg->peers.begin();
		 it != pg->peers.end();
		 it++) {
	  PGPeer *p = it->second;
	  messenger->send_message(new MOSDPGUpdate(osdmap->get_version(), pg->get_pgid(), false, osdmap->get_version()),
							  MSG_ADDR_OSD(p->get_peer()));
	}
  }

  // finish waiters
  if (pg->waiting_for_missing_object.count(o)) 
	take_waiters(pg->waiting_for_missing_object[o]);

  // more?
  do_recovery(pg);

  osd_lock.Unlock();
  
  delete op;
}


// push

void OSD::pg_push(PG *pg, int maxops)
{
  int ops = pg->num_active_ops();

  dout(7) << "pg_push pg " << hex << pg->get_pgid() << dec << " " << pg->objects_unrep.size() << " objects, " << ops << "/" << maxops << " active ops" <<  endl;
  
  while (ops < maxops) {
	object_t oid;
	if (!pg->get_next_push(oid)) {
	  dout(7) << "pg_push done " << *pg << endl;
	  break;
	}

	push_replica(pg, oid);
	ops++;
  }  
}

void OSD::push_replica(PG *pg, object_t oid)
{
  version_t v = 0;
  store->getattr(oid, "version", &v, sizeof(v));
  assert(v > 0);

  set<int>& peers = pg->objects_unrep[oid];

  // load object content
  struct stat st;
  store->stat(oid, &st);
  bufferlist bl;
  store->read(oid, st.st_size, 0, bl);
  assert(bl.length() == st.st_size);

  dout(7) << "push_replica " << hex << oid << dec << " v " << v << " to osds " << peers << "  size " << st.st_size << endl;

  for (set<int>::iterator pit = peers.begin();
	   pit != peers.end();
	   pit++) {
	PGPeer *p = pg->get_peer(*pit);
	assert(p);
	
	// add to list
	pg->pushing(oid, v, p);

	// send op
	__uint64_t tid = ++last_tid;
	MOSDOp *op = new MOSDOp(tid, messenger->get_myaddr(),
							oid, pg->get_pgid(),
							osdmap->get_version(),
							OSD_OP_REP_PUSH);
	op->set_version(v);
	op->set_pg_role(-1);  // whatever, not 0
	
	// include object content
	//op->set_data(bl);  // no no bad, will modify bl
	op->get_data() = bl;  // _copy_ bufferlist, we may have multiple destinations!
	op->set_length(st.st_size);
	op->set_offset(0);
	
	messenger->send_message(op, MSG_ADDR_OSD(*pit));

	// register
	push_ops[tid] = p;
  }

}

void OSD::op_rep_push(MOSDOp *op)
{
  dout(7) << "rep_push on " << hex << op->get_oid() << dec << " v " << op->get_version() <<  endl;
  lock_object(op->get_oid());

  PG *pg = get_pg(op->get_pg());
  assert(pg);

  // exists?
  if (store->exists(op->get_oid())) {
	store->truncate(op->get_oid(), 0);

	version_t ov = 0;
	store->getattr(op->get_oid(), "version", &ov, sizeof(ov));
	assert(ov <= op->get_version());
  }

  // write out buffers
  int r = store->write(op->get_oid(),
					   op->get_length(), 0,
					   op->get_data(),
					   false);       // FIXME
  pg->add_object(store, op->get_oid());
  assert(r >= 0);

  // set version
  version_t v = op->get_version();
  store->setattr(op->get_oid(), "version", &v, sizeof(v));

  // reply
  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, true);
  messenger->send_message(reply, op->get_asker());
  
  unlock_object(op->get_oid());
  delete op;
}

void OSD::op_rep_push_reply(MOSDOpReply *op)
{
  object_t oid = op->get_oid();
  version_t v = op->get_version();

  dout(7) << "rep_push_reply " << hex << oid << dec << endl;

  osd_lock.Lock();
  PGPeer *p = push_ops[op->get_tid()];
  PG *pg = p->pg;
  assert(p);   // FIXME: how will this work?
  assert(p->is_pushing(oid));
  assert(p->pushing_version(oid) == v);

  // close out push op.
  push_ops.erase(op->get_tid());
  pg->pushed(oid, v, p);

  if (p->is_complete()) {
	dout(7) << " telling replica they are complete" << endl;
	messenger->send_message(new MOSDPGUpdate(osdmap->get_version(), pg->get_pgid(), true, osdmap->get_version()),
							MSG_ADDR_OSD(p->get_peer()));
  }

  // anybody waiting on this object?
  if (pg->waiting_for_clean_object.count(oid) &&
	  pg->objects_unrep.count(oid) == 0) {
	dout(7) << "kicking waiter on now replicated object " << hex << oid << dec << endl;
	take_waiters(pg->waiting_for_clean_object[oid]);
	pg->waiting_for_clean_object.erase(oid);
  }

  // more?
  do_recovery(pg);

  osd_lock.Unlock();
  
  delete op;
}


// clean

void OSD::pg_clean(PG *pg, int maxops)
{
  int ops = pg->num_active_ops();

  dout(7) << "pg_clean pg " << hex << pg->get_pgid() << dec << " " << pg->objects_stray.size() << " stray, " << ops << "/" << maxops << " active ops" <<  endl;
  
  while (ops < maxops) {
	object_t oid;
	if (!pg->get_next_remove(oid)) {
	  dout(7) << "pg_clean done " << *pg << endl;
	  break;
	}
	
	remove_replica(pg, oid);
	ops++;
  }  
}

void OSD::remove_replica(PG *pg, object_t oid)
{
  dout(7) << "remove_replica " << hex << oid << dec << endl;//" v " << v << " from osd" << p->get_peer() << endl;

  map<int,version_t>& stray = pg->objects_stray[oid];
  for (map<int, version_t>::iterator it = stray.begin();
	   it != stray.end();
	   it++) {
	PGPeer *p = pg->get_peer(it->first);
	assert(p);
	const version_t v = it->second;

	// add to list
	pg->removing(oid, v, p);

	// send op
	__uint64_t tid = ++last_tid;
	MOSDOp *op = new MOSDOp(tid, messenger->get_myaddr(),
							oid, p->pg->get_pgid(),
							osdmap->get_version(),
							OSD_OP_REP_REMOVE);
	op->set_version(v);
	op->set_pg_role(-1);  // whatever, not 0
	messenger->send_message(op, MSG_ADDR_OSD(p->get_peer()));
	
	// register
	remove_ops[tid] = p;
  }
}

void OSD::op_rep_remove(MOSDOp *op)
{
  dout(7) << "rep_remove on " << hex << op->get_oid() << dec << " v " << op->get_version() <<  endl;
  lock_object(op->get_oid());
  
  // sanity checks
  assert(store->exists(op->get_oid()));

  version_t v = 0;
  store->getattr(op->get_oid(), "version", &v, sizeof(v));
  assert(v == op->get_version());

  // remove
  store->collection_remove(op->get_pg(), op->get_oid());
  int r = store->remove(op->get_oid());
  assert(r == 0);

  // reply
  messenger->send_message(new MOSDOpReply(op, r, osdmap, true), 
						  op->get_asker());

  unlock_object(op->get_oid());
  delete op;
}

void OSD::op_rep_remove_reply(MOSDOpReply *op)
{
  object_t oid = op->get_oid();
  version_t v = op->get_version();
  dout(7) << "rep_remove_reply " << hex << oid << dec << endl;

  osd_lock.Lock();
  PGPeer *p = remove_ops[op->get_tid()];
  PG *pg = p->pg;
  assert(p);   // FIXME: how will this work?
  assert(p->is_removing(oid));
  assert(p->removing_version(oid) == v);
  
  // close out push op.
  remove_ops.erase(op->get_tid());
  pg->removed(oid, v, p);
  
  if (p->is_complete()) {
	dout(7) << " telling replica they are complete" << endl;
	messenger->send_message(new MOSDPGUpdate(osdmap->get_version(), pg->get_pgid(), true, osdmap->get_version()),
							MSG_ADDR_OSD(p->get_peer()));
  }

  // more?
  do_recovery(pg);

  osd_lock.Unlock();
  
  delete op;
}


class C_OSD_RepModifySync : public Context {
public:
  OSD *osd;
  MOSDOp *op;
  C_OSD_RepModifySync(OSD *o, MOSDOp *oo) : osd(o), op(oo) { }
  void finish(int r) {
	osd->op_rep_modify_sync(op);
  }
};

void OSD::op_rep_modify_sync(MOSDOp *op)
{
  osd_lock.Lock();
  dout(2) << "rep_modify_sync on op " << op << endl;
  MOSDOpReply *ack2 = new MOSDOpReply(op, 0, osdmap, true);
  messenger->send_message(ack2, op->get_asker());
  delete op;
  osd_lock.Unlock();
}

void OSD::op_rep_modify(MOSDOp *op)
{ 
  // when we introduce unordered messaging.. FIXME
  object_t oid = op->get_oid();
  version_t ov = 0;
  if (store->exists(oid)) 
	store->getattr(oid, "version", &ov, sizeof(ov));

  assert(op->get_old_version() == ov);

  // PG
  PG *pg = get_pg(op->get_pg());
  assert(pg);
  
  dout(12) << "rep_modify in " << *pg << " o " << hex << oid << dec << " v " << op->get_version() << " (i have " << ov << ")" << endl;

  int r = 0;
  Context *onsync = 0;
  if (op->get_op() == OSD_OP_REP_WRITE) {
	// write
	assert(op->get_data().length() == op->get_length());
	onsync = new C_OSD_RepModifySync(this, op);
	r = apply_write(op, op->get_version(), onsync);
	if (ov == 0) pg->add_object(store, oid);
  } else if (op->get_op() == OSD_OP_REP_DELETE) {
	// delete
	store->collection_remove(pg->get_pgid(), op->get_oid());
	r = store->remove(oid);
  } else if (op->get_op() == OSD_OP_REP_TRUNCATE) {
	// truncate
	r = store->truncate(oid, op->get_offset());
  } else assert(0);
  
  if (onsync) {
	// ack
	MOSDOpReply *ack = new MOSDOpReply(op, 0, osdmap, false);
	messenger->send_message(ack, op->get_asker());
  } else {
	// sync, safe
	MOSDOpReply *ack = new MOSDOpReply(op, 0, osdmap, true);
	messenger->send_message(ack, op->get_asker());
	delete op;
  }
}




// =========================================================
// OPS

void OSD::handle_op(MOSDOp *op)
{
  osd_lock.Lock();

  pg_t pgid = op->get_pg();
  PG *pg = get_pg(pgid);

  // what kind of op?
  if (!OSD_OP_IS_REP(op->get_op())) {
	// REGULAR OP (non-replication)

	// is our map version up to date?
	if (op->get_map_version() > osdmap->get_version()) {
	  // op's is newer
	  dout(7) << "op map " << op->get_map_version() << " > " << osdmap->get_version() << endl;
	  wait_for_new_map(op);
	  osd_lock.Unlock();
	  return;
	}

	// am i the primary?
	int acting_primary = osdmap->get_pg_acting_primary( pgid );
	
	if (acting_primary != whoami) {
	  if (acting_primary >= 0) {
		dout(7) << " acting primary is " << acting_primary << ", forwarding" << endl;
		messenger->send_message(op, MSG_ADDR_OSD(acting_primary), 0);
		logger->inc("fwd");
	  } else {
		dout(1) << "crashed pg " << *pg << endl;
		messenger->send_message(new MOSDOpReply(op, -EIO, osdmap, true),
								op->get_asker());
		delete op;
	  }
	  osd_lock.Unlock();
	  return;
	}

	// proxy?
	if (!pg) {
	  dout(7) << "hit non-existent pg " << hex << op->get_pg() << dec << ", waiting" << endl;
	  waiting_for_pg[pgid].push_back(op);
	  osd_lock.Unlock();
	  return;
	}
	else {
	  dout(7) << "handle_op " << op << " in " << *pg << endl;

	  // must be peered.
	  if (!pg->is_peered()) {
		dout(7) << "op_write " << *pg << " not peered (yet)" << endl;
		pg->waiting_for_peered.push_back(op);
		osd_lock.Unlock();
		return;
	  }

	  const object_t oid = op->get_oid();

	  if (!pg->is_complete( osdmap->get_version() )) {
		// consult PG object map
		if (pg->objects_missing.count(oid)) {
		  // need to pull
		  dout(7) << "need to pull object " << hex << oid << dec << endl;
		  if (!pg->objects_pulling.count(oid)) 
			pull_replica(pg, oid);
		  pg->waiting_for_missing_object[oid].push_back(op);
		  osd_lock.Unlock();
		  return;
		}
	  }	  

	  if (!pg->is_clean() &&
		  (op->get_op() == OSD_OP_WRITE ||
		   op->get_op() == OSD_OP_TRUNCATE ||
		   op->get_op() == OSD_OP_DELETE)) {
		// exists but not replicated?
		if (pg->objects_unrep.count(oid)) {
		  dout(7) << "object " << hex << oid << dec << " in " << *pg 
				  << " exists but not clean" << endl;
		  pg->waiting_for_clean_object[oid].push_back(op);
		  if (pg->objects_pushing.count(oid) == 0)
			push_replica(pg, oid);
		  osd_lock.Unlock();
		  return;
		}

		// just stray?
		//  FIXME: this is a bit to aggressive; includes inactive peers
		if (pg->objects_stray.count(oid)) {  
		  dout(7) << "object " << hex << oid << dec << " in " << *pg 
				  << " dne but is not clean" << endl;
		  pg->waiting_for_clean_object[oid].push_back(op);
		  if (pg->objects_removing.count(oid) == 0)
			remove_replica(pg, oid);
		  osd_lock.Unlock();
		  return;
		}
	  }
	}	
	
  } else {
	// REPLICATION OP
	if (pg) {
	  dout(7) << "handle_rep_op " << op << " in " << *pg << endl;
	} else {
	  dout(7) << "handle_rep_op " << op << " in pgid " << op->get_pg() << endl;
	}
    // check osd map
	if (op->get_map_version() != osdmap->get_version()) {
	  // make sure source is still primary
	  int curprimary = osdmap->get_pg_acting_primary(op->get_pg());
	  int myrole = osdmap->get_pg_acting_role(op->get_pg(), whoami);
	  
	  if (curprimary != MSG_ADDR_NUM(op->get_source()) ||
		  myrole <= 0) {
		dout(5) << "op map " << op->get_map_version() << " != " << osdmap->get_version() << ", primary changed on pg " << hex << op->get_pg() << dec << endl;
		MOSDOpReply *fail = new MOSDOpReply(op, -1, osdmap, false);
		messenger->send_message(fail, op->get_asker());
		osd_lock.Unlock();
		return;
	  } else {
		dout(5) << "op map " << op->get_map_version() << " != " << osdmap->get_version() << ", primary same on pg " << hex << op->get_pg() << dec << endl;
	  }
	}  
  }

  // queue op
  if (g_conf.osd_maxthreads < 1) {
	pending_ops++;
	do_op(op);
  } else
	queue_op(op);

  osd_lock.Unlock();
}

void OSD::queue_op(MOSDOp *op) {
  // inc pending count
  osd_lock.Lock();
  pending_ops++;
  osd_lock.Unlock();

  threadpool->put_op(op);
}


void doop(OSD *u, MOSDOp *p) {
  u->do_op(p);
}

void OSD::do_op(MOSDOp *op) 
{
  dout(12) << "do_op " << op << endl;

  logger->inc("op");

  // replication ops?
  if (OSD_OP_IS_REP(op->get_op())) {
	// replication/recovery
	switch (op->get_op()) {
	  // push/pull/remove
	case OSD_OP_REP_PULL:
	  op_rep_pull(op);
	  break;
	case OSD_OP_REP_PUSH:
	  op_rep_push(op);
	  break;
	case OSD_OP_REP_REMOVE:
	  op_rep_remove(op);
	  break;

	  // replica ops
	case OSD_OP_REP_WRITE:
	case OSD_OP_REP_TRUNCATE:
	case OSD_OP_REP_DELETE:
	  op_rep_modify(op);
	  break;
	default:
	  assert(0);	  
	}
  } else {
	// regular op
	switch (op->get_op()) {
	case OSD_OP_READ:
	  op_read(op);
	  break;
	case OSD_OP_STAT:
	  op_stat(op);
	  break;
	case OSD_OP_WRITE:
	case OSD_OP_DELETE:
	case OSD_OP_TRUNCATE:
	  op_modify(op);
	  break;
	default:
	  assert(0);
	}
  }

  //dout(12) << "finish op " << op << endl;

  // finish
  osd_lock.Lock();
  assert(pending_ops > 0);
  pending_ops--;
  if (pending_ops == 0 && waiting_for_no_ops)
	no_pending_ops.Signal();
  osd_lock.Unlock();
}

void OSD::wait_for_no_ops()
{
  osd_lock.Lock();
  if (pending_ops > 0) {
	dout(7) << "wait_for_no_ops - waiting for " << pending_ops << endl;
	waiting_for_no_ops = true;
	no_pending_ops.Wait(osd_lock);
	waiting_for_no_ops = false;
	assert(pending_ops == 0);
  } 
  dout(7) << "wait_for_no_ops - none" << endl;
  osd_lock.Unlock();
}







// ===============================
// OPS

// READ OPS

void OSD::op_read(MOSDOp *op)
{
  object_t oid = op->get_oid();
  lock_object(oid);
  
  // read into a buffer
  bufferlist bl;
  long got = store->read(oid, 
						 op->get_length(), op->get_offset(),
						 bl);
  // set up reply
  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, true); 
  if (got >= 0) {
	reply->set_result(0);
	reply->set_data(bl);
	reply->set_length(got);
  } else {
	reply->set_result(got);   // error
	reply->set_length(0);
  }
  
  dout(12) << "read got " << got << " / " << op->get_length() << " bytes from obj " << hex << oid << dec << endl;

  logger->inc("rd");
  if (got >= 0) logger->inc("rdb", got);
  
  // send it
  messenger->send_message(reply, op->get_asker());

  delete op;

  unlock_object(oid);
}

void OSD::op_stat(MOSDOp *op)
{
  object_t oid = op->get_oid();
  lock_object(oid);

  struct stat st;
  memset(&st, sizeof(st), 0);
  int r = store->stat(oid, &st);
  
  dout(3) << "stat on " << hex << oid << dec << " r = " << r << " size = " << st.st_size << endl;
	  
  MOSDOpReply *reply = new MOSDOpReply(op, r, osdmap, true);
  reply->set_object_size(st.st_size);
  messenger->send_message(reply, op->get_asker());
	  
  logger->inc("stat");
  delete op;

  unlock_object(oid);
}



// WRITE OPS

int OSD::apply_write(MOSDOp *op, version_t v, Context *onsync)
{
  // take buffers from the message
  bufferlist bl;
  bl = op->get_data();
  //bl.claim( op->get_data() );
  
  // write 
  if (onsync) {
	if (g_conf.fake_osd_sync) {
	  // fake a delayed sync
	  store->write(op->get_oid(),
				   op->get_length(),
				   op->get_offset(),
				   bl,
				   false);
	  g_timer.add_event_after(1.0,
							  onsync);
	} else {
	  // for real
	  store->write(op->get_oid(),
				   op->get_length(),
				   op->get_offset(),
				   bl,
				   onsync);
	}
  } else {
	// normal business
	store->write(op->get_oid(),
				 op->get_length(),
				 op->get_offset(),
				 bl,
				 false);
  }

  // set version
  store->setattr(op->get_oid(), "version", &v, sizeof(v));

  return 0;
}



void OSD::issue_replica_op(PG *pg, OSDReplicaOp *repop, int osd)
{
  MOSDOp *op = repop->op;
  object_t oid = op->get_oid();

  dout(7) << " issue_replica_op in " << *pg << " o " << hex << oid << dec << " to osd" << osd << endl;
  
  // forward the write
  __uint64_t tid = ++last_tid;
  MOSDOp *wr = new MOSDOp(tid,
						  messenger->get_myaddr(),
						  oid,
						  pg->get_pgid(),
						  osdmap->get_version(),
						  100+op->get_op());
  wr->get_data() = op->get_data();   // copy bufferlist
  wr->set_length(op->get_length());
  wr->set_offset(op->get_offset());
  wr->set_version(repop->new_version);
  wr->set_old_version(repop->old_version);
  wr->set_pg_role(1); // replica
  messenger->send_message(wr, MSG_ADDR_OSD(osd));
  
  repop->osds.insert(osd);
  repop->waitfor_ack[tid] = osd;
  repop->waitfor_sync[tid] = osd;

  replica_ops[tid] = repop;
  replica_pg_osd_tids[pg->get_pgid()][osd].insert(tid);
}


class C_OSD_WriteSync : public Context {
public:
  OSD *osd;
  OSDReplicaOp *repop;
  C_OSD_WriteSync(OSD *o, OSDReplicaOp *op) : osd(o), repop(op) {}
  void finish(int r) {
	osd->op_modify_sync(repop);
  }
};

void OSD::op_modify_sync(OSDReplicaOp *repop)
{
  dout(2) << "op_modify_sync on op " << repop->op << endl;

  osd_lock.Lock();
  {
	repop->local_sync = true;
	if (repop->can_send_sync()) {
	  dout(2) << "op_modify_sync on " << hex << repop->op->get_oid() << dec << " op " << repop->op << endl;
	  if (repop->op->wants_safe()) {
		MOSDOpReply *reply = new MOSDOpReply(repop->op, 0, osdmap, true);
		messenger->send_message(reply, repop->op->get_asker());
	  }
	  delete repop->op;
	}
	if (repop->can_delete()) {
	  delete repop;
	}
  }
  osd_lock.Unlock();
}

void OSD::op_modify(MOSDOp *op)
{
  object_t oid = op->get_oid();

  char *opname = 0;
  if (op->get_op() == OSD_OP_WRITE) opname = "op_write";
  if (op->get_op() == OSD_OP_DELETE) opname = "op_delete";
  if (op->get_op() == OSD_OP_TRUNCATE) opname = "op_truncate";

  lock_object(oid);

  // version?  clean?
  version_t ov = 0;  // 0 == dne (yet)
  store->getattr(oid, "version", &ov, sizeof(ov));
  version_t nv = messenger->peek_lamport();
  assert(nv > ov);

  dout(12) << opname << " " << hex << oid << dec << " v " << nv << "  off " << op->get_offset() << " len " << op->get_length() << endl;  

  // issue replica writes
  OSDReplicaOp *repop = new OSDReplicaOp(op, nv, ov);

  osd_lock.Lock();
  PG *pg = get_pg(op->get_pg());
  for (unsigned i=1; i<pg->acting.size(); i++) {
	issue_replica_op(pg, repop, pg->acting[i]);
  }
  osd_lock.Unlock();

  // pre-ack
  //MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, false);
  //messenger->send_message(reply, op->get_asker());
  
  // do it
  int r;
  if (op->get_op() == OSD_OP_WRITE) {
	// write
	assert(op->get_data().length() == op->get_length());
	Context *onsync = new C_OSD_WriteSync(this, repop);
	r = apply_write(op, nv, onsync);
	
	// put new object in proper collection
	if (ov == 0) pg->add_object(store, oid);

	repop->local_ack = true;
  } 
  else if (op->get_op() == OSD_OP_TRUNCATE) {
	// truncate
	r = store->truncate(oid, op->get_offset());
	repop->local_ack = true;
	repop->local_sync = true;
  }
  else if (op->get_op() == OSD_OP_DELETE) {
	// delete
	pg->remove_object(store, op->get_oid());
	r = store->remove(oid);
	repop->local_ack = true;
	repop->local_sync = true;
  }
  else assert(0);

  // can we reply yet?
  osd_lock.Lock();
  {
	if (repop->can_send_sync()) {
	  dout(10) << opname << " sending sync on " << op << endl;
	  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, true);
	  messenger->send_message(reply, op->get_asker());
	}
	else if (repop->can_send_ack()) {
	  dout(10) << opname << " sending ack on " << op << endl;
	  MOSDOpReply *reply = new MOSDOpReply(op, 0, osdmap, false);
	  messenger->send_message(reply, op->get_asker());
	}
  }
  osd_lock.Unlock();

  unlock_object(oid);
}



