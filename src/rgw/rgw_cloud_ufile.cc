#include "rgw_cloud_ufile.h"
#include "rgw_common.h"
#include "rgw_rest_client.h"
#include "rgw_rados.h"
#include "common/armor.h"
#include "common/ceph_crypto.h"
#include "common/ceph_json.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

class RGWRESTUfileRequest : public RGWRESTSimpleRequest {
  Mutex lock;
  RGWCloudInfo& cloud_info;
  uint64_t bl_len;
  uint64_t total_send;
  uint64_t pos;
  std::list<bufferptr>::const_iterator cur_it;
  std::list<bufferptr>::const_iterator end_it;

protected:
  RGWGetDataCB *cb;
  RGWHTTPManager http_manager;
private:
  void create_ufile_canonical_header(const std::string& method, const string& bucket, const string& key, const string& content_type, string& dest_str);
  int get_ufile_header_digest(const std::string& auth_hdr, const string& key, string& dest);
  int complete();
  int add_output_data(bufferlist& bl, uint64_t len);
  void reset(); 
public:
  int send_data(void *ptr, size_t len);
  
  RGWRESTUfileRequest(CephContext *_cct, const std::string& _url, param_vec_t *_headers,
                                 param_vec_t *_params, RGWCloudInfo& _cloud_info) 
                                   : RGWRESTSimpleRequest(_cct, _url, _headers, _params),
                                     lock("RGWRESTStreamWriteUfileRequest"), 
                                     cloud_info(_cloud_info), bl_len(0),total_send(0),pos(0),
                                     cb(NULL),
                                     http_manager(_cct)
  { }
  ~RGWRESTUfileRequest();
  
  RGWGetDataCB *get_out_cb() { return cb; }

  int put_obj(const std::string& bucket, const string& obj, bufferlist* bl, uint64_t obj_size);
  int rm_obj(const std::string& bucket, const string& obj);
  int create_bucket(const std::string& bucket);

  int init_upload_multipart(const std::string& bucket, const std::string& key, std::string& upload_id, uint64_t& block_size, int32_t& retcode);
  int upload_multipart(const std::string& bucket, const std::string& key, const std::string& uplaod_id, uint64_t seq, bufferlist& bl, uint64_t obj_size, std::string& etag);
  int finish_upload_multipart(const std::string& bucket, const std::string& key, const std::string& upload_id, std::map<uint64_t,std::string>& etags);
  int abort_upload_multipart(const std::string& bucket, const std::string& key, const std::string& upload_id);
};

RGWRESTUfileRequest::~RGWRESTUfileRequest() {
  delete cb;
  cb = nullptr; 
}

void RGWRESTUfileRequest::reset() {
  pos = 0;
  total_send = 0;
  cur_it = end_it;
}

int RGWRESTUfileRequest::add_output_data(bufferlist& bl, uint64_t len) {
  if (status < 0) {
    int ret = status;
    return ret;
  }
  cur_it = bl.buffers().begin();
  end_it = bl.buffers().end();
  bl_len = len;
  bool done;
  return http_manager.process_requests(false, &done);
}

int RGWRESTUfileRequest::send_data(void *ptr, size_t len) {
  uint64_t sent = 0;
  if (bl_len == 0 || status < 0) {
    dout(20) << "RGWRESTStreamWriteRequest::send_data status=" <<status << " len:"<<bl_len<<dendl;
    reset();
    return status;
  }
  
  uint64 remain_len = 0;
  while(cur_it != end_it && len>0) {
    remain_len = cur_it->length()-pos;
    uint64_t send_len = min(len, (size_t)remain_len);
    memcpy(ptr, cur_it->c_str() + pos, send_len);
    ptr = (char*)ptr + send_len;
    len -= send_len;
    sent += send_len;
    pos += send_len;
    total_send += send_len;
     
    if ( pos == cur_it->length() ) {
      ++cur_it;
      pos = 0;
    }
  }
  
  return sent;
}

int RGWRESTUfileRequest::complete() {
  int ret = http_manager.complete_requests();
  if (ret < 0)
    return ret;
  return status;
}

void RGWRESTUfileRequest::create_ufile_canonical_header(const std::string& method, const string& bucket, const string& key, const string& content_type, string& dest_str) {
  dest_str = method +"\n" + "\n" + content_type + "\n" +"\n" + "/"+bucket +"/" +key;
}

int RGWRESTUfileRequest::get_ufile_header_digest(const std::string& auth_hdr, const string& key, string& dest)
{
  if (key.empty())
    return -EINVAL;

  char hmac_sha1[CEPH_CRYPTO_HMACSHA1_DIGESTSIZE];
  calc_hmac_sha1(key.c_str(), key.size(), auth_hdr.c_str(), auth_hdr.size(), hmac_sha1);

  char encode_buf_64[64]; /* 64 is really enough */
  int ret = ceph_armor(encode_buf_64, encode_buf_64 + 64, hmac_sha1,
           hmac_sha1 + CEPH_CRYPTO_HMACSHA1_DIGESTSIZE);
  if (ret < 0) {
    dout(10) << "ceph_armor failed:" << ret << dendl;
    return ret;
  }
  encode_buf_64[ret] = '\0';

  dest = encode_buf_64;

  return 0;
}

int RGWRESTUfileRequest::init_upload_multipart(const std::string& bucket, const std::string& obj, 
                                     std::string& upload_id, uint64_t& block_size, int32_t& retcode) {
  std::string new_url = "http://" + bucket + "." + cloud_info.domain_name + "/" + obj + "?uploads";
  std::string string2sign;
  std::string sign;
  std::string auth;
  std::string content_type = "application/octet-stream";
  create_ufile_canonical_header("POST", bucket, obj,content_type, string2sign);
  get_ufile_header_digest(string2sign ,cloud_info.private_key, sign);
  auth = "UCloud ";
  auth.append(cloud_info.public_key);
  auth.append(":");
  auth.append(sign);

  headers.push_back(pair<std::string, std::string>("Authorization", auth));
  headers.push_back(pair<std::string, std::string>("Content-Type", content_type));
  retcode = 0;

  int r = http_manager.add_request(this, "POST", new_url.c_str());
  if (r < 0)
    return r;

  r = complete();
  auto& buflist = get_response();
  string resp(buflist.c_str(), buflist.length());
  if (r < 0) {
    std::string error_code;
    JSONParser json;
    bool ret = json.parse(resp.c_str(), resp.length());
    if (ret) {
      if (json.get_data("RetCode", &error_code))
        retcode = std::stoi(error_code);
    }
    return r;
  }

  JSONParser json;
  json.parse(resp.c_str(), resp.length());
  if (!json.get_data("UploadId", &upload_id)) {
    return -EINVAL;
  }
  std::string blk_size;
  if (!json.get_data("BlkSize", &blk_size)) {
    return -EINVAL;
  }
  block_size = std::stoll(blk_size);
  return r;
}

int RGWRESTUfileRequest::upload_multipart(const std::string& bucket, const std::string& obj, 
                                          const std::string& upload_id, uint64_t seq,
                                          bufferlist& bl, uint64_t obj_size, std::string& etag) {
  std::string new_url = "http://" + bucket + "." + cloud_info.domain_name + "/" + obj + 
                        "?uploadId=" + upload_id + "&partNumber="+ std::to_string(seq);
  std::string string2sign;
  std::string sign;
  std::string auth;
  std::string content_type = "application/octet-stream";
  create_ufile_canonical_header("PUT", bucket, obj,content_type, string2sign);
  get_ufile_header_digest(string2sign ,cloud_info.private_key, sign);
  auth = "UCloud ";
  auth.append(cloud_info.public_key);
  auth.append(":");
  auth.append(sign);

  headers.push_back(pair<std::string, std::string>("Authorization", auth));
  headers.push_back(pair<std::string, std::string>("Content-Type", content_type));
  
  set_send_length(obj_size);
  int r = http_manager.add_request(this, "PUT", new_url.c_str());
  if (r < 0)
    return r;
  
  add_output_data(bl, obj_size);
 
  r = complete();
  if (r < 0)
    return r;

  auto& headers = get_out_headers();
  auto it = headers.find("ETAG");
  if (it != headers.end()) {
    etag = it->second;
  }else {
    r = -EINVAL;
  }
  return r;
}

int RGWRESTUfileRequest::finish_upload_multipart(const std::string& bucket, const std::string& obj, 
                         const std::string& upload_id, std::map<uint64_t, std::string>& etags) {
  std::string new_url = "http://" + bucket + "." + cloud_info.domain_name + "/" + obj +
                        "?uploadId=" + upload_id;
  std::string string2sign;
  std::string sign;
  std::string auth;
  std::string content_type = "application/octet-stream";
  create_ufile_canonical_header("POST", bucket, obj,content_type, string2sign);
  get_ufile_header_digest(string2sign ,cloud_info.private_key, sign);
  auth = "UCloud ";
  auth.append(cloud_info.public_key);
  auth.append(":");
  auth.append(sign);

  headers.push_back(pair<std::string, std::string>("Authorization", auth));
  headers.push_back(pair<std::string, std::string>("Content-Type", content_type));

  int r = http_manager.add_request(this, "POST", new_url.c_str());
  if (r < 0)
    return r;
  
  if (etags.empty()) {
    return -EINVAL;
  }

  auto it = etags.begin();
  std::string etag = it->second;
  ++it;
  while(it != etags.end()) {
    etag += ",";
    etag += it->second;
    ++it;
  }

  bufferptr bp(etag.c_str(), etag.length());
  bufferlist data;
  data.push_back(bp);
  add_output_data(data, etag.length());

  r = complete();
  return r;
}

int RGWRESTUfileRequest::abort_upload_multipart(const std::string& bucket, const std::string& obj, const std::string& upload_id) {
  std::string new_url = "http://" + bucket + "." + cloud_info.domain_name + "/" + obj +
                        "?uploadId=" + upload_id;
  std::string string2sign;
  std::string sign;
  std::string auth;
  std::string content_type = "application/octet-stream";
  create_ufile_canonical_header("DELETE", bucket, obj,content_type, string2sign);
  get_ufile_header_digest(string2sign ,cloud_info.private_key, sign);
  auth = "UCloud ";
  auth.append(cloud_info.public_key);
  auth.append(":");
  auth.append(sign);

  headers.push_back(pair<std::string, std::string>("Authorization", auth));
  headers.push_back(pair<std::string, std::string>("Content-Type", content_type));

  int r = http_manager.add_request(this, "DELETE", new_url.c_str());
  if (r < 0)
    return r;

  r = complete();
  return r;
}

int RGWRESTUfileRequest::put_obj(const std::string& bucket, const string& obj, bufferlist* bl, uint64_t obj_size) {
  
  std::string new_url = "http://" + bucket + "." + cloud_info.domain_name + "/" + obj;
  
  std::string string2sign;
  std::string sign;
  std::string auth;
  std::string content_type = "application/octet-stream";
  create_ufile_canonical_header("PUT", bucket, obj,content_type, string2sign);
  get_ufile_header_digest(string2sign ,cloud_info.private_key, sign);
  auth = "UCloud ";
  auth.append(cloud_info.public_key);
  auth.append(":");
  auth.append(sign);

  headers.push_back(pair<std::string, std::string>("Authorization", auth));
  headers.push_back(pair<std::string, std::string>("Content-Type", content_type));

  set_send_length(obj_size);

  int r = http_manager.add_request(this, "PUT", new_url.c_str());
  if (r < 0) {
    dout(0)<<"ufile request put_obj http_manager.add_request ret:"<<r<<dendl;
    return r;
  }

  add_output_data(*bl, obj_size);
  
  r = complete();
  if (r < 0) {
    dout(0)<<"ufile request put_obj complete ret:"<<r<<dendl;
  }
  reset();
  return r;
}
    
int RGWRESTUfileRequest::rm_obj(const std::string& bucket, const string& obj) {

  std::string new_url = "http://" + bucket+ "." + cloud_info.domain_name + "/" + obj;

  std::string string2sign;
  std::string sign;
  std::string auth;
  std::string content_type = "application/octet-stream";
  create_ufile_canonical_header("DELETE",bucket, obj, content_type, string2sign);
  get_ufile_header_digest(string2sign, cloud_info.private_key, sign);
  auth = "UCloud ";
  auth.append(cloud_info.public_key);
  auth.append(":");
  auth.append(sign);

  headers.push_back(pair<std::string, std::string>("Authorization", auth));
  headers.push_back(pair<std::string, std::string>("Content-Type", content_type));

  int r = http_manager.add_request(this, "DELETE", new_url.c_str());
  if (r < 0)
    return r;
  
  r = complete();
  return r;
}

int RGWRESTUfileRequest::create_bucket(const std::string& bucket) {
  std::string url = "http://"+ cloud_info.bucket_host +"/?";
  std::map<std::string, std::string> querys;
  querys["BucketName"] = bucket;
  querys["PublicKey"] = cloud_info.public_key;
  querys["Action"] = "CreateBucket";
  querys["Type"] = "private";
  querys["Region"]= cloud_info.bucket_region;
  std::string str2sign;
  
  std::map<std::string, std::string>::iterator iter = querys.begin();
  while(iter != querys.end())
  {
    str2sign += (iter->first + iter->second);
    if (iter->first == "PublicKey") {
      std::string encode_value;
      url_encode(iter->second, encode_value);
      url += (iter->first +"="+ encode_value +"&");
    }
    else {
      url += (iter->first +"=" +iter->second+"&");
    }
    ++iter;
  }

  url += "Signature=";
  str2sign += cloud_info.private_key;
  
  ceph::crypto::SHA1 sha1;
  unsigned char out[20];
  char hex[40];
  sha1.Update((const unsigned char*)(str2sign.c_str()), str2sign.length());
  sha1.Final(out);
  for(int i=0; i<20; i++) {
    sprintf( ( hex + (2*i)), "%02x", out[i]&0xff);
  }
  
  url += std::string(hex, 40);

  int r = http_manager.add_request(this, "GET", url.c_str());
  if (r < 0)
    return r;
    
  r = complete();
  return r;
}

int RGWCloudUfile::init_multipart(const std::string& bucket, const string& key) {
  param_vec_t _headers;
  param_vec_t _params;
  std::string url = "";
  std::string dest_bucket = cloud_info.dest_bucket.empty() ? bucket : cloud_info.dest_bucket;
  dest_bucket = cloud_info.bucket_prefix + dest_bucket;

  int32_t retcode = 0;  

  RGWRESTUfileRequest* req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
  int ret = req->init_upload_multipart(dest_bucket, key, upload_id, block_size, retcode);
  delete req;
  if (ret >= 0)
    return ret;
 

  // if bucket not exist, create it
  if(retcode == RGWCloudUfile::UFILE_BUCKET_NOT_EXIST) {
    req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
    ret = req->create_bucket(dest_bucket);
    delete req;
    if(ret < 0) {
      dout(0) << "ufile create_bucket:"<<bucket<<" failed:" << ret<< dendl;
      return ret;
    }

    req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
    ret = req->init_upload_multipart(dest_bucket, key, upload_id, block_size, retcode);
    delete req;
    if (ret < 0) {
      dout(0) <<"ufile init upload multipart failed:"<<ret<<" bucket:"<<bucket<<" file:"<<key<<dendl;
    }
    return ret;
  }
  else {
    dout(0) << "ufile init upload multipart failed:" << ret<<" bucket:"<<bucket<<" file:"<<key<<dendl;
    return ret;
  }
}

int RGWCloudUfile::finish_multipart(const std::string& bucket, const string& key) {
  param_vec_t _headers;
  param_vec_t _params;
  std::string url = "";
  std::string dest_bucket = cloud_info.dest_bucket.empty() ? bucket : cloud_info.dest_bucket;
  dest_bucket = cloud_info.bucket_prefix + dest_bucket;

  RGWRESTUfileRequest* req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);  
  int ret = req->finish_upload_multipart(dest_bucket, key, upload_id, etags);

  delete req;
  if (ret < 0) {
    dout(0) << "cloud error finish upload multipart ret:" << ret << " "
                          << dest_bucket << " file:" << key << dendl;
    req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
    int r = req->abort_upload_multipart(dest_bucket, key, upload_id);
    delete req;
    if (r < 0) {
      dout(0) << "cloud error abort upload multipart when finish ret:" << ret << " "
                            << dest_bucket << " file:" << key << dendl;
    }
    return ret;
  }
  return ret;
}

int RGWCloudUfile::abort_multipart(const std::string& bucket, const string& key) {
  param_vec_t _headers;
  param_vec_t _params;
  std::string url = "";
  std::string dest_bucket = cloud_info.dest_bucket.empty() ? bucket : cloud_info.dest_bucket;
  dest_bucket = cloud_info.bucket_prefix + dest_bucket;

  RGWRESTUfileRequest* req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
  int ret = req->abort_upload_multipart(dest_bucket, key, upload_id);
  delete req;
  if (ret < 0) {
    dout(0) << "cloud error abort upload multipart when finish ret:" << ret << " "
                            << dest_bucket << " file:" << key << dendl;
  }
  return ret;
}

int RGWCloudUfile::upload_multipart(const std::string& bucket, const string& key, bufferlist& buf, uint64_t size) {
  param_vec_t _headers;
  param_vec_t _params;
  std::string url = "";
  std::string dest_bucket = cloud_info.dest_bucket.empty() ? bucket : cloud_info.dest_bucket;
  dest_bucket = cloud_info.bucket_prefix + dest_bucket;
  
  RGWRESTUfileRequest* req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
  string etag_part;
  int ret = req->upload_multipart(dest_bucket, key, upload_id, part_number, buf, size, etag_part);
  etags.insert(std::make_pair(part_number, etag_part));
  ++part_number;
  
  delete req;
  if (ret < 0) {
    dout(0) << "cloud error upload multipart ret:" << ret << " "
                          << dest_bucket << " file:" << key << dendl;
  }
  return ret;
}

int RGWCloudUfile::put_obj(const std::string& bucket, const string& key, bufferlist *bl, off_t bl_len) {
  param_vec_t _headers;
  param_vec_t _params;
  std::string url = "";
  std::string dest_bucket = cloud_info.dest_bucket.empty() ? bucket : cloud_info.dest_bucket;
  dest_bucket = cloud_info.bucket_prefix + dest_bucket;
  
  RGWRESTUfileRequest* req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
  int ret = req->put_obj(dest_bucket, key, bl, bl_len);
  if (ret >= 0) {
    delete req;
    return ret;
  }
  
  int retcode = 0;
  auto& buflist = req->get_response();
  string resp(buflist.c_str(), buflist.length());
  std::string error_code;
  JSONParser json;
  bool json_ret = json.parse(resp.c_str(), resp.length());
  if (json_ret) {
    if (json.get_data("RetCode", &error_code))
      retcode = std::stoi(error_code);
  }
  
  delete req;

  // if bucket not exist, create it
  if(retcode == RGWCloudUfile::UFILE_BUCKET_NOT_EXIST) {
    req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
    ret = req->create_bucket(dest_bucket);
    delete req;
    if(ret < 0) {
      dout(0) << "ufile create_bucket:"<<bucket<<" failed:" << ret<< dendl;
      return ret;
    }
    
    req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
    ret = req->put_obj(dest_bucket, key, bl, bl_len);
    delete req;
    if (ret < 0) {
      dout(0) <<"ufile put_obj failed:"<<ret<<" bucket:"<<bucket<<" file:"<<key<<dendl;
    }
  }
  else {
    dout(0) << "ufile put_obj failed:" << ret<<" bucket:"<<bucket<<" file:"<<key<<dendl;
  }
  return ret;
}

int RGWCloudUfile::remove_obj(const std::string& bucket, const string& key) {
  param_vec_t _headers;
  param_vec_t _params;
  std::string url = "";
  std::string dest_bucket = cloud_info.dest_bucket.empty() ? bucket : cloud_info.dest_bucket;
  dest_bucket = cloud_info.bucket_prefix + dest_bucket;
  RGWRESTUfileRequest* req = new RGWRESTUfileRequest(cct, url, &_headers, &_params, cloud_info);
  int ret = req->rm_obj(dest_bucket, key);
  if (ret == 0) {
    dout(0) << "ufile rm_obj:" << key << " success." << dendl;
    delete req;
    return ret; 
  }
   
  auto& resp = req->get_response();
  int resp_len = resp.length();
  if (resp_len <= 0) {
    delete req;
    return ret;
  }
  std::string msg(resp.c_str(), resp_len);
  delete req;

  dout(0) << "ufile rm_obj:"<< key << " failed:" << ret<< " error msg:" << msg << dendl;
  return ret;
}
  

