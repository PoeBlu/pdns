#include "lua-recursor4.hh"
#include <fstream>
#include "logger.hh"
#include "dnsparser.hh"
#include "syncres.hh"
#include "namespaces.hh"
#include "rec_channel.hh" 
#include "ednssubnet.hh"
#include <unordered_set>

#if !defined(HAVE_LUA)
RecursorLua4::RecursorLua4(const std::string &fname)
{
  throw std::runtime_error("Attempt to load a Lua script in a PowerDNS binary without Lua support");
}

bool RecursorLua4::nxdomain(const ComboAddress& remote,const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& ret, int& res, bool* variable)
{
  return false;
}

bool RecursorLua4::nodata(const ComboAddress& remote,const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& ret, int& res, bool* variable)
{
  return false;
}

bool RecursorLua4::postresolve(const ComboAddress& remote,const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& ret, std::string* appliedPolicy, std::vector<std::string>* policyTags, int& res, bool* variable)
{
  return false;
}

bool RecursorLua4::preresolve(const ComboAddress& remote, const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& ret, const vector<pair<uint16_t,string> >* ednsOpts, unsigned int tag, std::string* appliedPolicy, std::vector<std::string>* policyTags, int& res, bool* variable)
{
  return false;
}

bool RecursorLua4::preoutquery(const ComboAddress& remote, const ComboAddress& local,const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& ret, int& res)
{
  return false;
}

bool RecursorLua4::ipfilter(const ComboAddress& remote, const ComboAddress& local, const struct dnsheader& dh)
{
  return false;
}

int RecursorLua4::gettag(const ComboAddress& remote, const Netmask& ednssubnet, const ComboAddress& local, const DNSName& qname, uint16_t qtype, std::vector<std::string>* policyTags)
{
  return 0;
}


#else
#undef L
#include "ext/luawrapper/include/LuaContext.hpp"

static int followCNAMERecords(vector<DNSRecord>& ret, const QType& qtype)
{
  vector<DNSRecord> resolved;
  DNSName target;
  for(const DNSRecord& rr :  ret) {
    if(rr.d_type == QType::CNAME) {
      auto rec = getRR<CNAMERecordContent>(rr);
      if(rec) {
        target=rec->getTarget();
        break;
      }
    }
  }
  if(target.empty())
    return 0;
  
  int rcode=directResolve(target, qtype, 1, resolved); // 1 == class
  
  for(const DNSRecord& rr :  resolved) {
    ret.push_back(rr);
  }
  return rcode;
 
}

static int getFakeAAAARecords(const DNSName& qname, const std::string& prefix, vector<DNSRecord>& ret)
{
  int rcode=directResolve(qname, QType(QType::A), 1, ret);

  ComboAddress prefixAddress(prefix);

  for(DNSRecord& rr :  ret)
  {
    if(rr.d_type == QType::A && rr.d_place==DNSResourceRecord::ANSWER) {
      if(auto rec = getRR<ARecordContent>(rr)) {
        ComboAddress ipv4(rec->getCA());
        uint32_t tmp;
        memcpy((void*)&tmp, &ipv4.sin4.sin_addr.s_addr, 4);
        // tmp=htonl(tmp);
        memcpy(((char*)&prefixAddress.sin6.sin6_addr.s6_addr)+12, &tmp, 4);
        rr.d_content = std::make_shared<AAAARecordContent>(prefixAddress);
        rr.d_type = QType::AAAA;
      }
    }
  }
  return rcode;
}

static int getFakePTRRecords(const DNSName& qname, const std::string& prefix, vector<DNSRecord>& ret)
{
  /* qname has a reverse ordered IPv6 address, need to extract the underlying IPv4 address from it
     and turn it into an IPv4 in-addr.arpa query */
  ret.clear();
  vector<string> parts = qname.getRawLabels();

  if(parts.size() < 8)
    return -1;

  string newquery;
  for(int n = 0; n < 4; ++n) {
    newquery +=
      std::to_string(stoll(parts[n*2], 0, 16) + 16*stoll(parts[n*2+1], 0, 16));
    newquery.append(1,'.');
  }
  newquery += "in-addr.arpa.";


  int rcode = directResolve(DNSName(newquery), QType(QType::PTR), 1, ret);
  for(DNSRecord& rr :  ret)
  {
    if(rr.d_type == QType::PTR && rr.d_place==DNSResourceRecord::ANSWER) {
      rr.d_name = qname;
    }
  }
  return rcode;

}

vector<pair<uint16_t, string> > RecursorLua4::DNSQuestion::getEDNSOptions()
{
  if(ednsOptions)
    return *ednsOptions;
  else
    return vector<pair<uint16_t,string>>();
}

boost::optional<string>  RecursorLua4::DNSQuestion::getEDNSOption(uint16_t code)
{
  if(ednsOptions)
    for(const auto& o : *ednsOptions)
      if(o.first==code)
        return o.second;
        
  return boost::optional<string>();
}

boost::optional<Netmask>  RecursorLua4::DNSQuestion::getEDNSSubnet()
{

  if(ednsOptions) {
    for(const auto& o : *ednsOptions) {
      if(o.first==8) {
        EDNSSubnetOpts eso;
        if(getEDNSSubnetOptsFromString(o.second, &eso))
          return eso.source;
        else 
          break;
      }
    }
  }
  return boost::optional<Netmask>();
}


vector<pair<int, DNSRecord> > RecursorLua4::DNSQuestion::getRecords()
{
  vector<pair<int, DNSRecord> > ret;
  int num=1;
  for(const auto& r : records) {
    ret.push_back({num++, r});
  }
  return ret;
}
void RecursorLua4::DNSQuestion::setRecords(const vector<pair<int, DNSRecord> >& recs)
{
  records.clear();
  for(const auto& p : recs) {
    records.push_back(p.second);
    cout<<"Setting: "<<p.second.d_content->getZoneRepresentation()<<endl;
  }
}

void RecursorLua4::DNSQuestion::addRecord(uint16_t type, const std::string& content, DNSResourceRecord::Place place, boost::optional<int> ttl, boost::optional<string> name)
{
  DNSRecord dr;
  dr.d_name=name ? DNSName(*name) : qname;
  dr.d_ttl=ttl.get_value_or(3600);
  dr.d_type = type;
  dr.d_place = place;
  dr.d_content = shared_ptr<DNSRecordContent>(DNSRecordContent::mastermake(type, 1, content));
  records.push_back(dr);
}

void RecursorLua4::DNSQuestion::addAnswer(uint16_t type, const std::string& content, boost::optional<int> ttl, boost::optional<string> name)
{
  addRecord(type, content, DNSResourceRecord::ANSWER, ttl, name);
}

struct DynMetric
{
  std::atomic<unsigned long>* ptr;
  void inc() { (*ptr)++; }
  void incBy(unsigned int by) { (*ptr)+= by; }
  unsigned long get() { return *ptr; }
  void set(unsigned long val) { *ptr =val; }
};

RecursorLua4::RecursorLua4(const std::string& fname)
{
  d_lw = std::unique_ptr<LuaContext>(new LuaContext);

  d_lw->registerFunction<int(dnsheader::*)()>("getID", [](dnsheader& dh) { return dh.id; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getCD", [](dnsheader& dh) { return dh.cd; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getTC", [](dnsheader& dh) { return dh.tc; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getRA", [](dnsheader& dh) { return dh.ra; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getAD", [](dnsheader& dh) { return dh.ad; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getAA", [](dnsheader& dh) { return dh.aa; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getRD", [](dnsheader& dh) { return dh.rd; });
  d_lw->registerFunction<int(dnsheader::*)()>("getRCODE", [](dnsheader& dh) { return dh.rcode; });
  d_lw->registerFunction<int(dnsheader::*)()>("getOPCODE", [](dnsheader& dh) { return dh.opcode; });
  d_lw->registerFunction<int(dnsheader::*)()>("getQDCOUNT", [](dnsheader& dh) { return ntohs(dh.qdcount); });
  d_lw->registerFunction<int(dnsheader::*)()>("getANCOUNT", [](dnsheader& dh) { return ntohs(dh.ancount); });
  d_lw->registerFunction<int(dnsheader::*)()>("getNSCOUNT", [](dnsheader& dh) { return ntohs(dh.nscount); });
  d_lw->registerFunction<int(dnsheader::*)()>("getARCOUNT", [](dnsheader& dh) { return ntohs(dh.arcount); });

  d_lw->writeFunction("newDN", [](boost::variant<const std::string, const DNSName> dom){
    if(dom.which() == 0)
      return DNSName(boost::get<const std::string>(dom));
    else
      return DNSName(boost::get<const DNSName>(dom));
  });
  d_lw->registerFunction("isPartOf", &DNSName::isPartOf);
  d_lw->registerFunction<bool(DNSName::*)(const std::string&)>("equal",
							      [](const DNSName& lhs, const std::string& rhs) { return lhs==DNSName(rhs); });
  d_lw->registerFunction("__eq", &DNSName::operator==);

  d_lw->registerFunction<string(ComboAddress::*)()>("toString", [](const ComboAddress& ca) { return ca.toString(); });
  d_lw->registerFunction<string(ComboAddress::*)()>("toStringWithPort", [](const ComboAddress& ca) { return ca.toStringWithPort(); });
  d_lw->registerFunction<uint16_t(ComboAddress::*)()>("getPort", [](const ComboAddress& ca) { return ntohs(ca.sin4.sin_port); } );
  d_lw->registerFunction<string(ComboAddress::*)()>("getRaw", [](const ComboAddress& ca) { 
      if(ca.sin4.sin_family == AF_INET) {
        auto t=ca.sin4.sin_addr.s_addr; return string((const char*)&t, 4); 
      }
      else 
        return string((const char*)&ca.sin6.sin6_addr.s6_addr, 16);
    } );

  d_lw->writeFunction("newCA", [](const std::string& a) { return ComboAddress(a); });
  typedef std::unordered_set<ComboAddress,ComboAddress::addressOnlyHash,ComboAddress::addressOnlyEqual> cas_t;
  d_lw->writeFunction("newCAS", []{ return cas_t(); });


  d_lw->registerFunction<void(cas_t::*)(boost::variant<string,ComboAddress, vector<pair<unsigned int,string> > >)>("add",
                                                                                   [](cas_t& cas, const boost::variant<string,ComboAddress,vector<pair<unsigned int,string> > >& in)
										   {
										     try {
										     if(auto s = boost::get<string>(&in)) {
										       cas.insert(ComboAddress(*s));
										     }
										     else if(auto v = boost::get<vector<pair<unsigned int, string> > >(&in)) {
										       for(const auto& s : *v)
											 cas.insert(ComboAddress(s.second));
										     }
										     else
										       cas.insert(boost::get<ComboAddress>(in));
										     }
										     catch(std::exception& e) { theL() <<Logger::Error<<e.what()<<endl; }
										   });

  d_lw->registerFunction<bool(cas_t::*)(const ComboAddress&)>("check",[](const cas_t& cas, const ComboAddress&ca) {
      return (bool)cas.count(ca);
    });



  d_lw->registerFunction<bool(ComboAddress::*)(const ComboAddress&)>("equal", [](const ComboAddress& lhs, const ComboAddress& rhs) {
      return ComboAddress::addressOnlyEqual()(lhs, rhs);
    });
  

  d_lw->registerFunction<ComboAddress(Netmask::*)()>("getNetwork", [](const Netmask& nm) { return nm.getNetwork(); } ); // const reference makes this necessary
  d_lw->registerFunction<ComboAddress(Netmask::*)()>("getMaskedNetwork", [](const Netmask& nm) { return nm.getMaskedNetwork(); } );
  d_lw->registerFunction("isIpv4", &Netmask::isIpv4);
  d_lw->registerFunction("isIpv6", &Netmask::isIpv6);
  d_lw->registerFunction("getBits", &Netmask::getBits);
  d_lw->registerFunction("toString", &Netmask::toString);
  d_lw->registerFunction("empty", &Netmask::empty);
  d_lw->registerFunction("match", (bool (Netmask::*)(const string&) const)&Netmask::match);
  d_lw->registerFunction("__eq", &Netmask::operator==);

  d_lw->writeFunction("newNMG", []() { return NetmaskGroup(); });
  d_lw->registerFunction<void(NetmaskGroup::*)(const std::string&mask)>("addMask", [](NetmaskGroup&nmg, const std::string& mask)
			 {
			   nmg.addMask(mask);
			 });

  d_lw->registerFunction<void(NetmaskGroup::*)(const vector<pair<unsigned int, std::string>>&)>("addMasks", [](NetmaskGroup&nmg, const vector<pair<unsigned int, std::string>>& masks)
			 {
                           for(const auto& mask: masks) 
                             nmg.addMask(mask.second);
			 });


  d_lw->registerFunction("match", (bool (NetmaskGroup::*)(const ComboAddress&) const)&NetmaskGroup::match);
  d_lw->registerFunction<string(DNSName::*)()>("toString", [](const DNSName&dn ) { return dn.toString(); });
  d_lw->registerFunction<string(DNSName::*)()>("toStringNoDot", [](const DNSName&dn ) { return dn.toStringNoDot(); });
  d_lw->registerFunction<bool(DNSName::*)()>("chopOff", [](DNSName&dn ) { return dn.chopOff(); });
  d_lw->registerMember("qname", &DNSQuestion::qname);
  d_lw->registerMember("qtype", &DNSQuestion::qtype);
  d_lw->registerMember("isTcp", &DNSQuestion::isTcp);
  d_lw->registerMember("localaddr", &DNSQuestion::local);
  d_lw->registerMember("remoteaddr", &DNSQuestion::remote);
  d_lw->registerMember("rcode", &DNSQuestion::rcode);
  d_lw->registerMember("tag", &DNSQuestion::tag);
  d_lw->registerMember("variable", &DNSQuestion::variable);
  d_lw->registerMember("followupFunction", &DNSQuestion::followupFunction);
  d_lw->registerMember("followupPrefix", &DNSQuestion::followupPrefix);
  d_lw->registerMember("followupName", &DNSQuestion::followupName);
  d_lw->registerMember("data", &DNSQuestion::data);
  d_lw->registerMember("udpQuery", &DNSQuestion::udpQuery);
  d_lw->registerMember("udpAnswer", &DNSQuestion::udpAnswer);
  d_lw->registerMember("udpQueryDest", &DNSQuestion::udpQueryDest);
  d_lw->registerMember("udpCallback", &DNSQuestion::udpCallback);
  d_lw->registerMember("appliedPolicy", &DNSQuestion::appliedPolicy);
  d_lw->registerFunction("getEDNSOptions", &DNSQuestion::getEDNSOptions);
  d_lw->registerFunction("getEDNSOption", &DNSQuestion::getEDNSOption);
  d_lw->registerFunction("getEDNSSubnet", &DNSQuestion::getEDNSSubnet);
  d_lw->registerMember("name", &DNSRecord::d_name);
  d_lw->registerMember("type", &DNSRecord::d_type);
  d_lw->registerMember("ttl", &DNSRecord::d_ttl);

  
  d_lw->registerFunction<string(DNSRecord::*)()>("getContent", [](const DNSRecord& dr) { return dr.d_content->getZoneRepresentation(); });
  d_lw->registerFunction<boost::optional<ComboAddress>(DNSRecord::*)()>("getCA", [](const DNSRecord& dr) { 
      boost::optional<ComboAddress> ret;

      if(auto rec = std::dynamic_pointer_cast<ARecordContent>(dr.d_content))
        ret=rec->getCA(53);
      else if(auto rec = std::dynamic_pointer_cast<AAAARecordContent>(dr.d_content))
        ret=rec->getCA(53);
      return ret;
    });


  d_lw->registerFunction<void(DNSRecord::*)(const std::string&)>("changeContent", [](DNSRecord& dr, const std::string& newContent) { dr.d_content = shared_ptr<DNSRecordContent>(DNSRecordContent::mastermake(dr.d_type, 1, newContent)); });
  d_lw->registerFunction("addAnswer", &DNSQuestion::addAnswer);
  d_lw->registerFunction("addRecord", &DNSQuestion::addRecord);
  d_lw->registerFunction("getRecords", &DNSQuestion::getRecords);
  d_lw->registerFunction("setRecords", &DNSQuestion::setRecords);

  d_lw->registerFunction<void(DNSQuestion::*)(const std::string&)>("addPolicyTag", [](DNSQuestion& dq, const std::string& tag) { if (dq.policyTags) { dq.policyTags->push_back(tag); } });
  d_lw->registerFunction<void(DNSQuestion::*)(const std::vector<std::pair<int, std::string> >&)>("setPolicyTags", [](DNSQuestion& dq, const std::vector<std::pair<int, std::string> >& tags) {
      if (dq.policyTags) {
        dq.policyTags->clear();
        for (const auto& tag : tags) {
          dq.policyTags->push_back(tag.second);
        }
      }
    });
  d_lw->registerFunction<std::vector<std::pair<int, std::string> >(DNSQuestion::*)()>("getPolicyTags", [](const DNSQuestion& dq) {
      std::vector<std::pair<int, std::string> > ret;
      if (dq.policyTags) {
        int count = 1;
        for (const auto& tag : *dq.policyTags) {
          ret.push_back({count++, tag});
        }
      }
      return ret;
    });

  d_lw->writeFunction("newDS", []() { return SuffixMatchNode(); });
  d_lw->registerFunction<void(SuffixMatchNode::*)(boost::variant<string,DNSName, vector<pair<unsigned int,string> > >)>("add",
										   [](SuffixMatchNode&smn, const boost::variant<string,DNSName,vector<pair<unsigned int,string> > >& in)
										   {
										     try {
										     if(auto s = boost::get<string>(&in)) {
										       smn.add(DNSName(*s));
										     }
										     else if(auto v = boost::get<vector<pair<unsigned int, string> > >(&in)) {
										       for(const auto& s : *v)
											 smn.add(DNSName(s.second));
										     }
										     else
										       smn.add(boost::get<DNSName>(in));
										     }
										     catch(std::exception& e) { theL() <<Logger::Error<<e.what()<<endl; }
										   });
  d_lw->registerFunction("check",(bool (SuffixMatchNode::*)(const DNSName&) const) &SuffixMatchNode::check);
  d_lw->registerFunction("toString",(string (SuffixMatchNode::*)() const) &SuffixMatchNode::toString);


  d_lw->writeFunction("pdnslog", [](const std::string& msg, boost::optional<int> loglevel) {
      theL() << (Logger::Urgency)loglevel.get_value_or(Logger::Warning) << msg<<endl;
    });
  typedef vector<pair<string, int> > in_t;
  vector<pair<string, boost::variant<int, in_t, struct timeval* > > >  pd{
    {"PASS", (int)PolicyDecision::PASS}, {"DROP",  (int)PolicyDecision::DROP},
    {"TRUNCATE", (int)PolicyDecision::TRUNCATE}
  };

  vector<pair<string, int> > rcodes = {{"NOERROR",  RCode::NoError  },
                                       {"FORMERR",  RCode::FormErr  },
                                       {"SERVFAIL", RCode::ServFail },
                                       {"NXDOMAIN", RCode::NXDomain },
                                       {"NOTIMP",   RCode::NotImp   },
                                       {"REFUSED",  RCode::Refused  },
                                       {"YXDOMAIN", RCode::YXDomain },
                                       {"YXRRSET",  RCode::YXRRSet  },
                                       {"NXRRSET",  RCode::NXRRSet  },
                                       {"NOTAUTH",  RCode::NotAuth  },
                                       {"NOTZONE",  RCode::NotZone  }};
  for(const auto& rcode : rcodes)
    pd.push_back({rcode.first, rcode.second});

  pd.push_back({"loglevels", in_t{
        {"Alert", LOG_ALERT},
	{"Critical", LOG_CRIT},
	{"Debug", LOG_DEBUG},
        {"Emergency", LOG_EMERG},
	{"Info", LOG_INFO},
	{"Notice", LOG_NOTICE},
	{"Warning", LOG_WARNING},
	{"Error", LOG_ERR}
	  }});

  for(const auto& n : QType::names)
    pd.push_back({n.first, n.second});
  pd.push_back({"now", &g_now});
  d_lw->registerMember("tv_sec", &timeval::tv_sec);
  d_lw->registerMember("tv_usec", &timeval::tv_usec);

  d_lw->writeVariable("pdns", pd);

  d_lw->writeFunction("getMetric", [](const std::string& str) {
      return DynMetric{getDynMetric(str)};
    });

  d_lw->registerFunction("inc", &DynMetric::inc);
  d_lw->registerFunction("incBy", &DynMetric::incBy);
  d_lw->registerFunction("set", &DynMetric::set);
  d_lw->registerFunction("get", &DynMetric::get);
  
  
  ifstream ifs(fname);
  if(!ifs) {
    throw std::runtime_error("Unable to read configuration file from '"+fname+"': "+strerror(errno));
  }  	
  d_lw->executeCode(ifs);

  d_preresolve = d_lw->readVariable<boost::optional<luacall_t>>("preresolve").get_value_or(0);
  d_nodata = d_lw->readVariable<boost::optional<luacall_t>>("nodata").get_value_or(0);
  d_nxdomain = d_lw->readVariable<boost::optional<luacall_t>>("nxdomain").get_value_or(0);
  d_postresolve = d_lw->readVariable<boost::optional<luacall_t>>("postresolve").get_value_or(0);
  d_preoutquery = d_lw->readVariable<boost::optional<luacall_t>>("preoutquery").get_value_or(0);

  d_ipfilter = d_lw->readVariable<boost::optional<ipfilter_t>>("ipfilter").get_value_or(0);
  d_gettag = d_lw->readVariable<boost::optional<gettag_t>>("gettag").get_value_or(0);
}

bool RecursorLua4::preresolve(const ComboAddress& remote,const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& res, const vector<pair<uint16_t,string> >* ednsOpts, unsigned int tag, std::string* appliedPolicy, std::vector<std::string>* policyTags, int& ret, bool* variable)
{
  return genhook(d_preresolve, remote, local, query, qtype, isTcp, res, ednsOpts, tag, appliedPolicy, policyTags, ret, variable);
}

bool RecursorLua4::nxdomain(const ComboAddress& remote,const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& res, int& ret, bool* variable)
{
  return genhook(d_nxdomain, remote, local, query, qtype, isTcp, res, 0, 0, nullptr, nullptr, ret, variable);
}

bool RecursorLua4::nodata(const ComboAddress& remote,const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& res, int& ret, bool* variable)
{
  return genhook(d_nodata, remote, local, query, qtype, isTcp, res, 0, 0, nullptr, nullptr, ret, variable);
}

bool RecursorLua4::postresolve(const ComboAddress& remote,const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& res, std::string* appliedPolicy, std::vector<std::string>* policyTags, int& ret, bool* variable)
{
  return genhook(d_postresolve, remote, local, query, qtype, isTcp, res, 0, 0, appliedPolicy, policyTags, ret, variable);
}

bool RecursorLua4::preoutquery(const ComboAddress& ns, const ComboAddress& requestor, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& res, int& ret)
{
  return genhook(d_preoutquery, ns, requestor, query, qtype, isTcp, res, 0, 0, nullptr, nullptr, ret, 0);
}

bool RecursorLua4::ipfilter(const ComboAddress& remote, const ComboAddress& local, const struct dnsheader& dh)
{
  if(d_ipfilter)
    return d_ipfilter(remote, local, dh);
  return false; // don't block
}

int RecursorLua4::gettag(const ComboAddress& remote, const Netmask& ednssubnet, const ComboAddress& local, const DNSName& qname, uint16_t qtype, std::vector<std::string>* policyTags)
{
  if(d_gettag) {
    auto ret = d_gettag(remote, ednssubnet, local, qname, qtype);

    if (policyTags) {
      const auto& tags = std::get<1>(ret);
      if (tags) {
        for (const auto& tag : *tags) {
          policyTags->push_back(tag.second);
        }
      }
    }
    return std::get<0>(ret);
  }
  return 0;
}

bool RecursorLua4::genhook(luacall_t& func, const ComboAddress& remote,const ComboAddress& local, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& res, const vector<pair<uint16_t,string> >* ednsOpts, unsigned int tag, std::string* appliedPolicy, std::vector<std::string>* policyTags, int& ret, bool* variable)
{
  if(!func)
    return false;

  auto dq = std::make_shared<DNSQuestion>();
  dq->qname = query;
  dq->qtype = qtype.getCode();
  dq->local=local;
  dq->remote=remote;
  dq->records = res;
  dq->tag = tag;
  dq->ednsOptions = ednsOpts;
  dq->isTcp = isTcp;
  dq->policyTags = policyTags;
  bool handled=func(dq);
  if(variable) *variable |= dq->variable; // could still be set to indicate this *name* is variable, even if not 'handled'

  if(handled) {
loop:;
    ret=dq->rcode;
    
    if(!dq->followupFunction.empty()) {
      if(dq->followupFunction=="followCNAMERecords") {
        ret = followCNAMERecords(dq->records, qtype);
      }
      else if(dq->followupFunction=="getFakeAAAARecords") {
        ret=getFakeAAAARecords(dq->followupName, dq->followupPrefix, dq->records);
      }
      else if(dq->followupFunction=="getFakePTRRecords") {
        ret=getFakePTRRecords(dq->followupName, dq->followupPrefix, dq->records);
      }
      else if(dq->followupFunction=="udpQueryResponse") {
        dq->udpAnswer = GenUDPQueryResponse(dq->udpQueryDest, dq->udpQuery);
        auto func = d_lw->readVariable<boost::optional<luacall_t>>(dq->udpCallback).get_value_or(0);
        if(!func) {
          theL()<<Logger::Error<<"Attempted callback for Lua UDP Query/Response which could not be found"<<endl;
          return false;
        }
        bool res=func(dq);
        if(variable) *variable |= dq->variable; // could still be set to indicate this *name* is variable
        if(!res) {
          return false;
        }
        goto loop;
      }
    }
    res=dq->records;
    if (appliedPolicy) {
      *appliedPolicy=dq->appliedPolicy;
    }
  }


  // see if they added followup work for us too
  return handled;
}

#endif
RecursorLua4::~RecursorLua4(){}
