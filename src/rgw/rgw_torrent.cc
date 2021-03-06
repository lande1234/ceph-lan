#include <errno.h>
#include <stdlib.h>

#include <sstream>

#include "rgw_torrent.h"
#include "include/str_list.h"
#include "include/rados/librados.hpp"

#define dout_subsys ceph_subsys_rgw

using namespace std;
using ceph::crypto::MD5;
using namespace librados;
using namespace boost;
using ceph::crypto::SHA1;

seed::seed()
{
  seed::info.piece_length = 0;
  seed::info.len = 0;
  sha_len = 0;
  is_torrent = false; 
}

seed::~seed()
{
  seed::info.sha1_bl.clear();
  bl.clear();
  torrent_bl.clear();
  s = NULL;
  store = NULL;
}

void seed::init(struct req_state *p_req, RGWRados *p_store)
{
  s = p_req;
  store = p_store;
}

void seed::get_torrent_file(int &op_ret, RGWRados::Object::Read &read_op, uint64_t &total_len, 
  bufferlist &bl_data, rgw_obj &obj)
{
  /* add other field if config is set */
  dencode.bencode_dict(bl);
  set_announce();
  if (!comment.empty())
  {
    dencode.bencode(COMMENT, comment, bl);
  }
  if (!create_by.empty())
  {
    dencode.bencode(CREATED_BY, create_by, bl);
  }
  if (!encoding.empty())
  {
    dencode.bencode(ENCODING, encoding, bl);
  }

  string oid, key;
  rgw_bucket bucket;
  map<string, bufferlist> m;
  set<string> obj_key;
  get_obj_bucket_and_oid_loc(obj, bucket, oid, key);
  ldout(s->cct, 0) << "NOTICE: head obj oid= " << oid << dendl;

  obj_key.insert(RGW_OBJ_TORRENT);
  op_ret = read_op.state.io_ctx.omap_get_vals_by_keys(oid, obj_key, &m);
  if (op_ret < 0)
  {
    ldout(s->cct, 0) << "ERROR: failed to omap_get_vals_by_keys op_ret = " << op_ret << dendl;
    return;
  }

  map<string, bufferlist>::iterator iter;
  for (iter = m.begin(); iter != m.end(); ++iter)
  {
    bufferlist bl_tmp = iter->second;
    char *pbuff = bl_tmp.c_str();
    bl.append(pbuff, bl_tmp.length());
  }
  dencode.bencode_end(bl);

  bl_data = bl;
  total_len = bl.length();
  return;
}

bool seed::get_flag()
{
  return is_torrent;
}

void seed::save_data(bufferlist &bl)
{
  if (!is_torrent)
  {
    return;
  }

  info.len += bl.length();
  torrent_bl.push_back(bl);
}

off_t seed::get_data_len()
{
  return info.len;
}

void seed::set_create_date(ceph::real_time& value)
{
  utime_t date = ceph::real_clock::to_timespec(value);
  create_date = date.sec();
}

void seed::set_info_pieces(char *buff)
{
  info.sha1_bl.append(buff, CEPH_CRYPTO_SHA1_DIGESTSIZE);
}

void seed::set_info_name(const string& value)
{
  info.name = value;
}

void seed::sha1(SHA1 *h, bufferlist &bl, off_t bl_len)
{
  off_t num = bl_len/info.piece_length;
  off_t remain = 0;
  remain = bl_len%info.piece_length;

  char *pstr = bl.c_str();
  char sha[25];

  /* get sha1 */
  for (off_t i = 0; i < num; i++)
  {
    memset(sha, 0x00, sizeof(sha));
    h->Update((byte *)pstr, info.piece_length);
    h->Final((byte *)sha);
    set_info_pieces(sha);
    pstr += info.piece_length;
  }

  /* process remain */
  if (0 != remain)
  {
    memset(sha, 0x00, sizeof(sha));
    h->Update((byte *)pstr, remain);
    h->Final((byte *)sha);
    set_info_pieces(sha);
  }
}

int seed::sha1_process()
{
  uint64_t remain = info.len%info.piece_length;
  uint8_t  remain_len = ((remain > 0)? 1 : 0);
  sha_len = (info.len/info.piece_length + remain_len)*CEPH_CRYPTO_SHA1_DIGESTSIZE;

  SHA1 h;
  list<bufferlist>::iterator iter = torrent_bl.begin();
  for (; iter != torrent_bl.end(); iter++)
  {
    bufferlist &bl_info = *iter;
    sha1(&h, bl_info, (*iter).length());
  }

  return 0;
}

int seed::handle_data()
{
  int ret = 0;

  /* sha1 process */
  ret = sha1_process();
  if (0 != ret)
  {
    ldout(s->cct, 0) << "ERROR: failed to sha1_process() ret= "<< ret << dendl;
    return ret;
  }

  /* produce torrent data */
  do_encode();

  /* save torrent data into OMAP */
  ret = save_torrent_file();
  if (0 != ret)
  {
    ldout(s->cct, 0) << "ERROR: failed to save_torrent_file() ret= "<< ret << dendl;
    return ret;
  }

  return 0;
}

int seed::get_params()
{
  is_torrent = true;
  info.piece_length = g_conf->rgw_torrent_sha_unit;
  create_by = g_conf->rgw_torrent_createby;
  encoding = g_conf->rgw_torrent_encoding;
  origin = g_conf->rgw_torrent_origin;
  comment = g_conf->rgw_torrent_comment;
  announce = g_conf->rgw_torrent_tracker;

  /* tracker and tracker list is empty, set announce to origin */
  if (announce.empty() && !origin.empty())
  {
    announce = origin;
  }

  return 0;
}

void seed::set_announce()
{
  list<string> announce_list;  // used to get announce list from conf
  get_str_list(announce, ",", announce_list);

  if (announce_list.empty())
  {
    ldout(s->cct, 5) << "NOTICE: announce_list is empty " << dendl;    
    return;
  }

  list<string>::iterator iter = announce_list.begin();
  dencode.bencode_key(ANNOUNCE, bl);
  dencode.bencode_key((*iter), bl);

  dencode.bencode_key(ANNOUNCE_LIST, bl);
  dencode.bencode_list(bl);
  for (; iter != announce_list.end(); ++iter)
  {
    dencode.bencode_list(bl);
    dencode.bencode_key((*iter), bl);
    dencode.bencode_end(bl);
  }
  dencode.bencode_end(bl);
}

void seed::do_encode()
{ 
  /*Only encode create_date and sha1 info*/
  /*Other field will be added if confi is set when run get torrent*/
  dencode.bencode(CREATION_DATE, create_date, bl);

  dencode.bencode_key(INFO_PIECES, bl);
  dencode.bencode_dict(bl);  
  dencode.bencode(LENGTH, info.len, bl);
  dencode.bencode(NAME, info.name, bl);
  dencode.bencode(PIECE_LENGTH, info.piece_length, bl);

  char info_sha[100] = { 0 };
  sprintf(info_sha, "%ld", sha_len);
  string sha_len_str = info_sha;
  dencode.bencode_key(PIECES, bl);
  bl.append(sha_len_str.c_str(), sha_len_str.length());
  bl.append(':');
  bl.append(info.sha1_bl.c_str(), sha_len);
  dencode.bencode_end(bl);
}

int seed::save_torrent_file()
{
  int op_ret = 0;
  string key = RGW_OBJ_TORRENT;
  rgw_obj obj(s->bucket, s->object.name);    

  op_ret = store->omap_set(obj, key, bl);
  if (op_ret < 0)
  {
    ldout(s->cct, 0) << "ERROR: failed to omap_set() op_ret = " << op_ret << dendl;
    return op_ret;
  }

  return op_ret;
}
