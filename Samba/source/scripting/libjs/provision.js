/*
	backend code for provisioning a Samba4 server
	Copyright Andrew Tridgell 2005
	Released under the GNU GPL v2 or later
*/

sys = sys_init();

/*
  return true if the current install seems to be OK
*/
function install_ok(session_info, credentials)
{
	var lp = loadparm_init();
	var ldb = ldb_init();
	ldb.session_info = session_info;
	ldb.credentials = credentials;
	if (lp.get("realm") == "") {
		return false;
	}
	var ok = ldb.connect(lp.get("sam database"));
	if (!ok) {
		return false;
	}
	var res = ldb.search("(cn=Administrator)");
	if (res.length != 1) {
		return false;
	}
	return true;
}

/*
  find a user or group from a list of possibilities
*/
function findnss()
{
	var i;
	assert(arguments.length >= 2);
	var nssfn = arguments[0];
	for (i=1;i<arguments.length;i++) {
		if (nssfn(arguments[i]) != undefined) {
			return arguments[i];
		}
	}
	printf("Unable to find user/group for %s\n", arguments[1]);
	assert(i<arguments.length);
}

/*
   add a foreign security principle
 */
function add_foreign(ldb, subobj, sid, desc)
{
	var add = sprintf("
dn: CN=%s,CN=ForeignSecurityPrincipals,%s
objectClass: top
objectClass: foreignSecurityPrincipal
description: %s
",
			  sid, subobj.BASEDN, desc);
	/* deliberately ignore errors from this, as the records may
	   already exist */
	ldb.add(add);
}


/*
  setup a mapping between a sam name and a unix name
 */
function setup_name_mapping(info, ldb, sid, unixname)
{
	var attrs = new Array("dn");
	var res = ldb.search(sprintf("objectSid=%s", sid), 
			     info.subobj.BASEDN, ldb.SCOPE_SUBTREE, attrs);
	if (res.length != 1) {
		info.message("Failed to find record for objectSid %s\n", sid);
		return false;
	}
	var mod = sprintf("
dn: %s
changetype: modify
replace: unixName
unixName: %s
",
			  res[0].dn, unixname);
	var ok = ldb.modify(mod);
	if (!ok) {
		info.message("name mapping for %s failed - %s\n",
			     sid, ldb.errstring());
		return false;
	}
	return true;
}

/*
  return current time as a nt time string
*/
function nttime()
{
	return "" + sys.nttime();
}

/*
  return current time as a ldap time string
*/
function ldaptime()
{
	return sys.ldaptime(sys.nttime());
}

/*
  return a date string suitable for a dns zone serial number
*/
function datestring()
{
	var t = sys.ntgmtime(sys.nttime());
	return sprintf("%04u%02u%02u%02u",
		       t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour);
}

/*
  return first host IP
*/
function hostip()
{
	var list = sys.interfaces();
	return list[0];
}


/*
  return first part of hostname
*/
function hostname()
{
	var s = split(".", sys.hostname());
	return s[0];
}

/* the ldb is in bad shape, possibly due to being built from an
   incompatible previous version of the code, so delete it
   completely */
function ldb_delete(ldb)
{
	println("Deleting " + ldb.filename);
	var lp = loadparm_init();
	sys.unlink(sprintf("%s/%s", lp.get("private dir"), ldb.filename));
	ldb.transaction_cancel();
	ldb.close();
	var ok = ldb.connect(ldb.filename);
	ldb.transaction_start();
	assert(ok);
}

/*
  erase an ldb, removing all records
*/
function ldb_erase(ldb)
{
	var res;

	/* delete the specials */
	ldb.del("@INDEXLIST");
	ldb.del("@ATTRIBUTES");
	ldb.del("@SUBCLASSES");
	ldb.del("@MODULES");
	ldb.del("@PARTITION");
	ldb.del("@KLUDGEACL");

	/* and the rest */
	attrs = new Array("dn");
     	var basedn = "";
     	var res = ldb.search("(&(|(objectclass=*)(dn=*))(!(dn=@BASEINFO)))", basedn, ldb.SCOPE_SUBTREE, attrs);
	var i;
	if (typeof(res) == "undefined") {
		ldb_delete(ldb);
		return;
	}
	for (i=0;i<res.length;i++) {
		ldb.del(res[i].dn);
	}


     	var res = ldb.search("(&(|(objectclass=*)(dn=*))(!(dn=@BASEINFO)))", basedn, ldb.SCOPE_SUBTREE, attrs);
	if (res.length != 0) {
		ldb_delete(ldb);
		return;
	}
	assert(res.length == 0);
}

/*
  erase an ldb, removing all records
*/
function ldb_erase_partitions(info, ldb)
{
	var rootDSE_attrs = new Array("namingContexts");
	var lp = loadparm_init();
	var j;

	var res = ldb.search("(objectClass=*)", "", ldb.SCOPE_BASE, rootDSE_attrs);
	assert(typeof(res) != "undefined");
	assert(res.length == 1);
	if (typeof(res[0].namingContexts) == "undefined") {
		return;
	}	
	for (j=0; j<res[0].namingContexts.length; j++) {
		var attrs = new Array("dn");
		var basedn = res[0].namingContexts[j];
		var k;
		var previous_remaining = 1;
		var current_remaining = 0;

			for (k=0; k < 10 && (previous_remaining != current_remaining); k++) {
			/* and the rest */
			var res2 = ldb.search("(|(objectclass=*)(dn=*))", basedn, ldb.SCOPE_SUBTREE, attrs);
			var i;
			if (typeof(res2) == "undefined") {
				info.message("ldb search failed: " + ldb.errstring() + "\n");
				continue;
			}
			previous_remaining = current_remaining;
			current_remaining = res2.length;
			for (i=0;i<res2.length;i++) {
				ldb.del(res2[i].dn);
			}
			
			var res3 = ldb.search("(|(objectclass=*)(dn=*))", basedn, ldb.SCOPE_SUBTREE, attrs);
			if (typeof(res3) == "undefined") {
				info.message("ldb search failed: " + ldb.errstring() + "\n");
				continue;
			}
			if (res3.length != 0) {
				info.message("Failed to delete all records under " + basedn + ", " + res3.length + " records remaining\n");
			}
		}
	}
}

function open_ldb(info, dbname, erase)
{
	var ldb = ldb_init();
	ldb.session_info = info.session_info;
	ldb.credentials = info.credentials;
	ldb.filename = dbname;

	var connect_ok = ldb.connect(dbname);
	if (!connect_ok) {
		var lp = loadparm_init();
		sys.unlink(sprintf("%s/%s", lp.get("private dir"), dbname));
		connect_ok = ldb.connect(dbname);
		assert(connect_ok);
	}

	ldb.transaction_start();

	if (erase) {
		ldb_erase(ldb);	
	}
	return ldb;
}


/*
  setup a ldb in the private dir
 */
function setup_add_ldif(ldif, info, ldb, failok)
{
	var lp = loadparm_init();
	var src = lp.get("setup directory") + "/" + ldif;

	var data = sys.file_load(src);
	data = substitute_var(data, info.subobj);

	var add_ok = ldb.add(data);
	if (!add_ok) {
		info.message("ldb load failed: " + ldb.errstring() + "\n");
		if (!failok) {
			assert(add_ok);
	        }
	}
	return add_ok;
}

function setup_modify_ldif(ldif, info, ldb, failok)
{
	var lp = loadparm_init();
	var src = lp.get("setup directory") + "/" + ldif;

	var data = sys.file_load(src);
	data = substitute_var(data, info.subobj);

	var mod_ok = ldb.modify(data);
	if (!mod_ok) {
		info.message("ldb load failed: " + ldb.errstring() + "\n");
		if (!failok) {
			assert(mod_ok);
	        }
	}
	return mod_ok;
}


function setup_ldb(ldif, info, dbname) 
{
	var erase = true;
	var failok = false;

	if (arguments.length >= 4) {
	        erase = arguments[3];
        }
	if (arguments.length == 5) {
	        failok = arguments[4];
        }
	var ldb = open_ldb(info, dbname, erase);
	if (setup_add_ldif(ldif, info, ldb, failok)) {
		var commit_ok = ldb.transaction_commit();
		if (!commit_ok) {
			info.message("ldb commit failed: " + ldb.errstring() + "\n");
			assert(commit_ok);
		}
	}
}

/*
  setup a ldb in the private dir
 */
function setup_ldb_modify(ldif, info, ldb)
{
	var lp = loadparm_init();

	var src = lp.get("setup directory") + "/" + ldif;

	var data = sys.file_load(src);
	data = substitute_var(data, info.subobj);

	var mod_ok = ldb.modify(data);
	if (!mod_ok) {
		info.message("ldb load failed: " + ldb.errstring() + "\n");
		return mod_ok;
	}
	return mod_ok;
}

/*
  setup a file in the private dir
 */
function setup_file(template, message, fname, subobj)
{
	var lp = loadparm_init();
	var f = fname;
	var src = lp.get("setup directory") + "/" + template;

	sys.unlink(f);

	var data = sys.file_load(src);
	data = substitute_var(data, subobj);

	ok = sys.file_save(f, data);
	if (!ok) {
		message("failed to create file: " + f + "\n");
		assert(ok);
	}
}

function provision_default_paths(subobj)
{
	var lp = loadparm_init();
	var paths = new Object();
	paths.smbconf = lp.get("config file");
	paths.shareconf = lp.get("private dir") + "/" + "share.ldb";
	paths.hklm = "hklm.ldb";
	paths.hkcu = "hkcu.ldb";
	paths.hkcr = "hkcr.ldb";
	paths.hku = "hku.ldb";
	paths.hkpd = "hkpd.ldb";
	paths.hkpt = "hkpt.ldb";
	paths.samdb = lp.get("sam database");
	paths.secrets = "secrets.ldb";
	paths.dns = lp.get("private dir") + "/" + subobj.DNSDOMAIN + ".zone";
	paths.winsdb = "wins.ldb";
	paths.ldap_basedn_ldif = lp.get("private dir") + "/" + subobj.DNSDOMAIN + ".ldif";
	return paths;
}


/*
  setup reasonable name mappings for sam names to unix names
*/
function setup_name_mappings(info, ldb)
{
	var lp = loadparm_init();
	var attrs = new Array("objectSid");
	var subobj = info.subobj;

	res = ldb.search("objectSid=*", subobj.BASEDN, ldb.SCOPE_BASE, attrs);
	assert(res.length == 1 && res[0].objectSid != undefined);
	var sid = res[0].objectSid;

	/* add some foreign sids if they are not present already */
	add_foreign(ldb, subobj, "S-1-5-7",  "Anonymous");
	add_foreign(ldb, subobj, "S-1-1-0",  "World");
	add_foreign(ldb, subobj, "S-1-5-2",  "Network");
	add_foreign(ldb, subobj, "S-1-5-18", "System");
	add_foreign(ldb, subobj, "S-1-5-11", "Authenticated Users");

	/* some well known sids */
	setup_name_mapping(info, ldb, "S-1-5-7",  subobj.NOBODY);
	setup_name_mapping(info, ldb, "S-1-1-0",  subobj.NOGROUP);
	setup_name_mapping(info, ldb, "S-1-5-2",  subobj.NOGROUP);
	setup_name_mapping(info, ldb, "S-1-5-18", subobj.ROOT);
	setup_name_mapping(info, ldb, "S-1-5-11", subobj.USERS);
	setup_name_mapping(info, ldb, "S-1-5-32-544", subobj.WHEEL);
	setup_name_mapping(info, ldb, "S-1-5-32-545", subobj.USERS);
	setup_name_mapping(info, ldb, "S-1-5-32-546", subobj.NOGROUP);
	setup_name_mapping(info, ldb, "S-1-5-32-551", subobj.BACKUP);

	/* and some well known domain rids */
	setup_name_mapping(info, ldb, sid + "-500", subobj.ROOT);
	setup_name_mapping(info, ldb, sid + "-518", subobj.WHEEL);
	setup_name_mapping(info, ldb, sid + "-519", subobj.WHEEL);
	setup_name_mapping(info, ldb, sid + "-512", subobj.WHEEL);
	setup_name_mapping(info, ldb, sid + "-513", subobj.USERS);
	setup_name_mapping(info, ldb, sid + "-520", subobj.WHEEL);

	return true;
}


/*
  provision samba4 - caution, this wipes all existing data!
*/
function provision(subobj, message, blank, paths, session_info, credentials)
{
	var lp = loadparm_init();
	var sys = sys_init();
	var info = new Object();

	/*
	  some options need to be upper/lower case
	*/
	subobj.REALM       = strupper(subobj.REALM);
	subobj.HOSTNAME    = strlower(subobj.HOSTNAME);
	subobj.DOMAIN      = strupper(subobj.DOMAIN);
	assert(valid_netbios_name(subobj.DOMAIN));
	subobj.NETBIOSNAME = strupper(subobj.HOSTNAME);
	assert(valid_netbios_name(subobj.NETBIOSNAME));
	var rdns = split(",", subobj.BASEDN);
	subobj.RDN_DC = substr(rdns[0], strlen("DC="));
	
	if (subobj.DOMAINGUID != undefined) {
		subobj.DOMAINGUID_MOD = sprintf("replace: objectGUID\nobjectGUID: %s\n-", subobj.DOMAINGUID);
	} else {
		subobj.DOMAINGUID_MOD = "";
	}

	if (subobj.HOSTGUID != undefined) {
		subobj.HOSTGUID_ADD = sprintf("objectGUID: %s", subobj.HOSTGUID);
	} else {
		subobj.HOSTGUID_ADD = "";
	}

	info.subobj = subobj;
	info.message = message;
	info.credentials = credentials;
	info.session_info = session_info;

	/* only install a new smb.conf if there isn't one there already */
	var st = sys.stat(paths.smbconf);
	if (st == undefined) {
		message("Setting up smb.conf\n");
		setup_file("provision.smb.conf", info.message, paths.smbconf, subobj);
		lp.reload();
	}
	/* only install a new shares config db if there is none */
	st = sys.stat(paths.shareconf);
	if (st == undefined) {
		message("Setting up share.ldb\n");
		setup_ldb("share.ldif", info, paths.shareconf);
	}
	message("Setting up secrets.ldb\n");
	setup_ldb("secrets.ldif", info, paths.secrets);
	message("Setting up keytabs\n");
	var keytab_ok = credentials_update_all_keytabs();
	assert(keytab_ok);
	message("Setting up hklm.ldb\n");
	setup_ldb("hklm.ldif", info, paths.hklm);

	message("Setting up sam.ldb partitions\n");
	/* Also wipes the database */
	setup_ldb("provision_partitions.ldif", info, paths.samdb);

	var samdb = open_ldb(info, paths.samdb, false);

	message("Setting up sam.ldb attributes\n");
	setup_add_ldif("provision_init.ldif", info, samdb, false);
	message("Erasing data from partitions\n");
	ldb_erase_partitions(info, samdb);
	
	message("Adding baseDN: " + subobj.BASEDN + " (permitted to fail)\n");
	var add_ok = setup_add_ldif("provision_basedn.ldif", info, samdb, true);
	message("Modifying baseDN: " + subobj.BASEDN + "\n");
	var modify_ok = setup_ldb_modify("provision_basedn_modify.ldif", info, samdb);
	if (!modify_ok) {
		if (!add_ok) {
			message("Failed to both add and modify " + subobj.BASEDN + " in target " + subobj.LDAPBACKEND + "\n");
			message("Perhaps you need to run the provision script with the --ldap-base-dn option, and add this record to the backend manually\n"); 
		};
		assert(modify_ok);
	};

	message("Setting up sam.ldb Samba4 schema\n");
	setup_add_ldif("schema_samba4.ldif", info, samdb, false);
	message("Setting up sam.ldb AD schema\n");
	setup_add_ldif("schema.ldif", info, samdb, false);

	// (hack) Reload, now we have the schema loaded.  
	var commit_ok = samdb.transaction_commit();
	if (!commit_ok) {
		info.message("samdb commit failed: " + samdb.errstring() + "\n");
		assert(commit_ok);
	}
	samdb.close();

	samdb = open_ldb(info, paths.samdb, false);

	message("Setting up display specifiers\n");
	setup_add_ldif("display_specifiers.ldif", info, samdb, false);
	message("Setting up sam.ldb templates\n");
	setup_add_ldif("provision_templates.ldif", info, samdb, false);
	message("Setting up sam.ldb data\n");
	setup_add_ldif("provision.ldif", info, samdb, false);

	if (blank != false) {
		message("Setting up sam.ldb index\n");
		setup_add_ldif("provision_index.ldif", info, samdb, false);

		var commit_ok = samdb.transaction_commit();
		if (!commit_ok) {
			info.message("ldb commit failed: " + samdb.errstring() + "\n");
			assert(commit_ok);
		}
		return true;
	}

//	message("Activate schema module");
//	setup_modify_ldif("schema_activation.ldif", info, samdb, false);
//
//	// (hack) Reload, now we have the schema loaded.  
//	var commit_ok = samdb.transaction_commit();
//	if (!commit_ok) {
//		info.message("samdb commit failed: " + samdb.errstring() + "\n");
//		assert(commit_ok);
//	}
//	samdb.close();
//
//	samdb = open_ldb(info, paths.samdb, false);
//
	message("Setting up sam.ldb users and groups\n");
	setup_add_ldif("provision_users.ldif", info, samdb, false);

	if (setup_name_mappings(info, samdb) == false) {
		return false;
	}

	message("Setting up sam.ldb index\n");
	setup_add_ldif("provision_index.ldif", info, samdb, false);

	var commit_ok = samdb.transaction_commit();
	if (!commit_ok) {
		info.message("samdb commit failed: " + samdb.errstring() + "\n");
		assert(commit_ok);
	}

	return true;
}

/* Write out a DNS zone file, from the info in the current database */
function provision_dns(subobj, message, paths, session_info, credentials)
{
	message("Setting up DNS zone: " + subobj.DNSDOMAIN + " \n");
	var ldb = ldb_init();
	ldb.session_info = session_info;
	ldb.credentials = credentials;

	/* connect to the sam */
	var ok = ldb.connect(paths.samdb);
	assert(ok);

	/* These values may have changed, due to an incoming SamSync,
           or may not have been specified, so fetch them from the database */

	var attrs = new Array("objectGUID");
	res = ldb.search("objectGUID=*", subobj.BASEDN, ldb.SCOPE_BASE, attrs);
	assert(res.length == 1);
	assert(res[0].objectGUID != undefined);
	subobj.DOMAINGUID = res[0].objectGUID;

	subobj.HOSTGUID = searchone(ldb, subobj.BASEDN, "(&(objectClass=computer)(cn=" + subobj.NETBIOSNAME + "))", "objectGUID");
	assert(subobj.HOSTGUID != undefined);

	setup_file("provision.zone", 
		   message, paths.dns, 
		   subobj);

	message("Please install the zone located in " + paths.dns + " into your DNS server\n");
}

/* Write out a DNS zone file, from the info in the current database */
function provision_ldapbase(subobj, message, paths)
{
	message("Setting up LDAP base entry: " + subobj.BASEDN + " \n");
	var rdns = split(",", subobj.BASEDN);
	subobj.EXTENSIBLEOBJECT = "objectClass: extensibleObject";

	subobj.RDN_DC = substr(rdns[0], strlen("DC="));

	setup_file("provision_basedn.ldif", 
		   message, paths.ldap_basedn_ldif, 
		   subobj);

	message("Please install the LDIF located in " + paths.ldap_basedn_ldif + " into your LDAP server, and re-run with --ldap-backend=ldap://my.ldap.server\n");
}


/*
  guess reasonably default options for provisioning
*/
function provision_guess()
{
	var subobj = new Object();
	var nss = nss_init();
	var lp = loadparm_init();
	var rdn_list;
	random_init(local);

	subobj.REALM        = strupper(lp.get("realm"));
	subobj.DOMAIN       = lp.get("workgroup");
	subobj.HOSTNAME     = hostname();

	assert(subobj.REALM);
	assert(subobj.DOMAIN);
	assert(subobj.HOSTNAME);
	
	subobj.VERSION      = version();
	subobj.HOSTIP       = hostip();
	subobj.DOMAINSID    = randsid();
	subobj.INVOCATIONID = randguid();
	subobj.KRBTGTPASS   = randpass(12);
	subobj.MACHINEPASS  = randpass(12);
	subobj.ADMINPASS    = randpass(12);
	subobj.DEFAULTSITE  = "Default-First-Site-Name";
	subobj.NEWGUID      = randguid;
	subobj.NTTIME       = nttime;
	subobj.LDAPTIME     = ldaptime;
	subobj.DATESTRING   = datestring;
	subobj.ROOT         = findnss(nss.getpwnam, "root");
	subobj.NOBODY       = findnss(nss.getpwnam, "nobody");
	subobj.NOGROUP      = findnss(nss.getgrnam, "nogroup", "nobody");
	subobj.WHEEL        = findnss(nss.getgrnam, "wheel", "root", "staff", "adm");
	subobj.BACKUP       = findnss(nss.getgrnam, "backup", "wheel", "root", "staff");
	subobj.USERS        = findnss(nss.getgrnam, "users", "guest", "other", "unknown", "usr");
	subobj.DNSDOMAIN    = strlower(subobj.REALM);
	subobj.DNSNAME      = sprintf("%s.%s", 
				      strlower(subobj.HOSTNAME), 
				      subobj.DNSDOMAIN);
	rdn_list = split(".", subobj.DNSDOMAIN);
	subobj.BASEDN       = "DC=" + join(",DC=", rdn_list);
	subobj.LDAPBACKEND  = "users.ldb";
	subobj.LDAPMODULES = "objectguid";
	subobj.EXTENSIBLEOBJECT = "# no objectClass: extensibleObject for local ldb";
	return subobj;
}

/*
  search for one attribute as a string
 */
function searchone(ldb, basedn, expression, attribute)
{
	var attrs = new Array(attribute);
	res = ldb.search(expression, basedn, ldb.SCOPE_SUBTREE, attrs);
	if (res.length != 1 ||
	    res[0][attribute] == undefined) {
		return undefined;
	}
	return res[0][attribute];
}

/*
  modify an account to remove the 
*/
function enable_account(ldb, user_dn)
{
	var attrs = new Array("userAccountControl");
	var res = ldb.search(NULL, user_dn, ldb.SCOPE_ONELEVEL, attrs);
	assert(res.length == 1);
	var userAccountControl = res[0].userAccountControl;
	userAccountControl = userAccountControl - 2; /* remove disabled bit */
	var mod = sprintf("
dn: %s
changetype: modify
replace: userAccountControl
userAccountControl: %u
", 
			  user_dn, userAccountControl);
	var ok = ldb.modify(mod);
	return ok;	
}


/*
  add a new user record
*/
function newuser(username, unixname, password, message, session_info, credentials)
{
	var lp = loadparm_init();
	var samdb = lp.get("sam database");
	var ldb = ldb_init();
	random_init(local);
	ldb.session_info = session_info;
	ldb.credentials = credentials;

	/* connect to the sam */
	var ok = ldb.connect(samdb);
	assert(ok);

	ldb.transaction_start();

	/* find the DNs for the domain and the domain users group */
	var attrs = new Array("defaultNamingContext");
	res = ldb.search("defaultNamingContext=*", "", ldb.SCOPE_BASE, attrs);
	assert(res.length == 1 && res[0].defaultNamingContext != undefined);
	var domain_dn = res[0].defaultNamingContext;
	assert(domain_dn != undefined);
	var dom_users = searchone(ldb, domain_dn, "name=Domain Users", "dn");
	assert(dom_users != undefined);

	var user_dn = sprintf("CN=%s,CN=Users,%s", username, domain_dn);


	/*
	  the new user record. note the reliance on the samdb module to fill
	  in a sid, guid etc
	*/
	var ldif = sprintf("
dn: %s
sAMAccountName: %s
memberOf: %s
unixName: %s
sambaPassword: %s
objectClass: user
",
			   user_dn, username, dom_users,
			   unixname, password);
	/*
	  add the user to the users group as well
	*/
	var modgroup = sprintf("
dn: %s
changetype: modify
add: member
member: %s
", 
			       dom_users, user_dn);


	/*
	  now the real work
	*/
	message("Adding user %s\n", user_dn);
	ok = ldb.add(ldif);
	if (ok != true) {
		message("Failed to add %s - %s\n", user_dn, ldb.errstring());
		return false;
	}

	message("Modifying group %s\n", dom_users);
	ok = ldb.modify(modgroup);
	if (ok != true) {
		message("Failed to modify %s - %s\n", dom_users, ldb.errstring());
		return false;
	}

	/*
	  modify the userAccountControl to remove the disabled bit
	*/
	ok = enable_account(ldb, user_dn);
	if (ok) {
		ldb.transaction_commit();
	}
	return ok;
}

// Check whether a name is valid as a NetBIOS name. 
// FIXME: There are probably more constraints here. 
// crh has a paragraph on this in his book (1.4.1.1)
function valid_netbios_name(name)
{
	if (strlen(name) > 13) return false;
	return true;
}

function provision_validate(subobj, message)
{
	var lp = loadparm_init();

	if (!valid_netbios_name(subobj.DOMAIN)) {
		message("Invalid NetBIOS name for domain\n");
		return false;
	}

	if (!valid_netbios_name(subobj.NETBIOSNAME)) {
		message("Invalid NetBIOS name for host\n");
		return false;
	}


	if (strupper(lp.get("workgroup")) != strupper(subobj.DOMAIN)) {
		message("workgroup '%s' in smb.conf must match chosen domain '%s'\n",
			lp.get("workgroup"), subobj.DOMAIN);
		return false;
	}

	if (strupper(lp.get("realm")) != strupper(subobj.REALM)) {
		message("realm '%s' in smb.conf must match chosen realm '%s'\n",
			lp.get("realm"), subobj.REALM);
		return false;
	}

	return true;
}

function join_domain(domain, netbios_name, join_type, creds, message) 
{
	var ctx = NetContext(creds);
	var joindom = new Object();
	joindom.domain = domain;
	joindom.join_type = join_type;
	joindom.netbios_name = netbios_name;
	if (!ctx.JoinDomain(joindom)) {
		message("Domain Join failed: " + joindom.error_string);
		return false;
	}
	return true;
}

/* Vampire a remote domain.  Session info and credentials are required for for
 * access to our local database (might be remote ldap)
 */ 

function vampire(domain, session_info, credentials, message) {
	var ctx = NetContext(credentials);
	var vampire_ctx = new Object();
	var machine_creds = credentials_init();
	machine_creds.set_domain(form.DOMAIN);
	if (!machine_creds.set_machine_account()) {
		message("Failed to access domain join information!");
		return false;
	}
	vampire_ctx.machine_creds = machine_creds;
	vampire_ctx.session_info = session_info;
	if (!ctx.SamSyncLdb(vampire_ctx)) {
		message("Migration of remote domain to Samba failed: " + vampire_ctx.error_string);
		return false;
	}

	return true;
}

return 0;
