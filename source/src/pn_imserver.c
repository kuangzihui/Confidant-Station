/*************************************************************************
 *
 *  im server文件
 *
 * 
 * 
 * 
 * 
 *
 * 
 * 
 * 
 * 
 *
 * 
 * 
 *************************************************************************/
#include <libwebsockets.h>
#include <string.h>
#include <cJSON.h>
#include "common_lib.h"
#include "sql_db.h"
#include "pn_imserver.h"   
#include <pthread.h>
#include "upload.h"
#include "crc32.h"
#include "aes.h"

#define LWS_PLUGIN_STATIC
//#include "protocol_lws_minimal.c"
#if !defined (LWS_PLUGIN_STATIC)
#define LWS_DLL
#define LWS_INTERNAL
#include <libwebsockets.h>
#endif

#include <string.h>
#include "common_lib.h"
#include "tox_seg_msg.h"
#include "net_crypto.h"
#include "version.h"

#ifdef DEV_ONESPACE
int g_pnrdevtype = PNR_DEV_TYPE_ONESPACE;
#elif defined (DEV_RASIPI3)
int g_pnrdevtype = PNR_DEV_TYPE_RASIPI3;
#elif defined (DEV_EXPRESSOBIN)
int g_pnrdevtype = PNR_DEV_TYPE_EXPRESSOBIN;
#else
int g_pnrdevtype = PNR_DEV_TYPE_X86SERVER;
#endif
/* one of these created for each message */
struct msg {
	void *payload; /* is malloc'd */
	size_t len;
};

/* one of these is created for each vhost our protocol is used with */
struct per_vhost_data__minimal {
	struct lws_context *context;
	struct lws_vhost *vhost;
	const struct lws_protocols *protocol;

	struct per_session_data__minimal *pss_list; /* linked-list of live pss*/
};

struct per_vhost_data__minimal_bin {
	struct lws_context *context;
	struct lws_vhost *vhost;
	const struct lws_protocols *protocol;

	struct per_session_data__minimal_bin *pss_list; /* linked-list of live pss*/
};


/* destroys the message when everyone has had a copy of it */

int im_sendfile_get_node_byfd(int fd, int userindex);
static int callback_minimal(struct lws *wsi, enum lws_callback_reasons reason,
	void *user, void *in, size_t len);
static int callback_pnr_bin(struct lws *wsi, enum lws_callback_reasons reason,
    void *user, void *in, size_t len);
int im_rcvmsg_deal_bin(struct per_session_data__minimal_bin *pss, char *pmsg,
	int msg_len, char *retmsg, int *retmsg_len, int *ret_flag, int *plws_index);
int imtox_send_file(Tox *tox, struct lws_cache_msg_struct *msg, int push);

struct lws * g_lws_handler[PNR_IMUSER_MAXNUM+1];
#define IMSERVER_BAD_MSG_RET   "bad msg"
#define IMSERVER_BAD_MSG_RETLEN   sizeof(IMSERVER_BAD_MSG_RET)
#define LWS_MSGBUFF_MAXLEN     (64*1024)  
#define LWS_PLUGIN_PROTOCOL_MINIMAL \
	{ \
		"lws-minimal", \
		callback_minimal, \
		sizeof(struct per_session_data__minimal), \
		LWS_MSGBUFF_MAXLEN, \
		0, NULL, 0 \
	}
     
static struct lws_protocols bin_protocols[] = 
{
	{ "http", lws_callback_http_dummy, 0, 0, },
	{
		"lws-pnr-bin",
		callback_pnr_bin,
		sizeof(struct per_session_data__minimal_bin),
		LWS_MSGBUFF_MAXLEN,
	},
    { NULL, NULL, 0, 0 } /* terminator */
};

static struct lws_protocols protocols[] = 
{
	{ "http", lws_callback_http_dummy, 0, 0, },
    LWS_PLUGIN_PROTOCOL_MINIMAL,
    { NULL, NULL, 0, 0 } /* terminator */
};

static int interrupted = 0;

/* make sure every file can be downloaded */
static const struct lws_protocol_vhost_options pvo_mime = {
	NULL,				/* "next" pvo linked-list */
	NULL,				/* "child" pvo linked-list */
	"*", 				/* file suffix to match */
	"application/octet-stream"		/* mimetype to use */
};

static const struct lws_http_mount mount = 
{
    /* .mount_next */       NULL,       /* linked-list "next" */
    /* .mountpoint */       "/",        /* mountpoint URL */
    /* .origin */           WS_SERVER_INDEX_FILEPATH,  /* serve from dir */
    /* .def */          	"index.html",   /* default filename */
    /* .protocol */         NULL,
    /* .cgienv */           NULL,
    /* .extra_mimetypes */  &pvo_mime,
    /* .interpret */        NULL,
    /* .cgi_timeout */      0,
    /* .cache_max_age */        0,
    /* .auth_mask */        0,
    /* .cache_reusable */       0,
    /* .cache_revalidate */     0,
    /* .cache_intermediaries */ 0,
    /* .origin_protocol */      LWSMPRO_FILE,   /* files in a dir */
    /* .mountpoint_len */       1,      /* char count */
    /* .basic_auth_login_file */    NULL,
};

//added by willcao
int g_p2pnet_init_flag = FALSE;//tox p2p网络初始化完成标识
char g_devadmin_loginkey[PNR_LOGINKEY_MAXLEN+1] = {0};
char g_dev_nickname[PNR_USERNAME_MAXLEN+1] = {0};
struct im_user_struct g_daemon_tox;
struct im_user_array_struct g_imusr_array;
Tox* g_tox_linknode[PNR_IMUSER_MAXNUM+1];
int g_noticepost_enable = TRUE;
int g_msglist_debugswitch = FALSE;
//int g_tmp_instance_index = 0;
//toxdata信息数组
struct pnr_tox_datafile_struct g_tox_datafile[PNR_IMUSER_MAXNUM+1];
struct pnr_account_array_struct g_account_array;
struct pnr_rid_node g_rnode[PNR_IMUSER_MAXNUM+1];

//用户锁
pthread_mutex_t g_pnruser_lock[PNR_IMUSER_MAXNUM+1];
//用户tox数据文件锁
pthread_mutex_t g_user_toxdatalock[PNR_IMUSER_MAXNUM+1];
//用户消息id锁
pthread_mutex_t g_user_msgidlock[PNR_IMUSER_MAXNUM+1];
//用户好友列表锁
pthread_mutex_t g_user_friendlock[PNR_IMUSER_MAXNUM+1];

//lws消息队列，每个用户对应一个消息列表
struct lws_msg_struct g_lws_msglist[PNR_IMUSER_MAXNUM+1];
pthread_mutex_t lws_msglock[PNR_IMUSER_MAXNUM+1];

//lws缓存消息队列，得到确认后从队列中删除消息，否则重发
struct lws_cache_msg_struct g_lws_cache_msglist[PNR_IMUSER_MAXNUM+1];
pthread_mutex_t lws_cache_msglock[PNR_IMUSER_MAXNUM+1];

//tox消息队列，也是每个用户对应一个消息列表
struct imuser_toxmsg_struct g_tox_msglist[PNR_IMUSER_MAXNUM+1];
pthread_mutex_t tox_msglock[PNR_IMUSER_MAXNUM+1];

// valid disk modes
char *g_valid_disk_mode[] = {
	"NULL","BASIC", "RAID1", "RAID0", "LVM", "RAIDADD", "LVMADD", NULL
};
pthread_mutex_t g_formating_lock = PTHREAD_MUTEX_INITIALIZER;
char g_formating = 0;
int g_format_reboot_time = 0;
char *g_qrcode_head[] = {
	"type_0,", "type_1,", "type_2,", "type_3,",NULL
};

//群组相关结构
struct group_info g_grouplist[PNR_GROUP_MAXNUM+1];
pthread_mutex_t g_grouplock[PNR_GROUP_MAXNUM+1];
pthread_mutex_t g_grouptoplock = PTHREAD_MUTEX_INITIALIZER;
int g_groupnum = 0;
int g_group_invitee_needack = FALSE;
extern sqlite3 *g_groupdb_handle;
extern sqlite3 *g_db_handle;
extern sqlite3 *g_msglogdb_handle[PNR_IMUSER_MAXNUM+1];
extern sqlite3 *g_msgcachedb_handle[PNR_IMUSER_MAXNUM+1];
extern Tox *qlinkNode;
extern char g_dev_hwaddr_full[MACSTR_MAX_LEN];
extern char g_dev1_hwaddr_full[MACSTR_MAX_LEN];
extern int g_debug_level;
extern pthread_mutex_t g_toxmsg_cache_lock[PNR_IMUSER_MAXNUM+1];
int CreatedP2PNetwork_new(int user_index);
void im_send_file_by_tox(Tox* tox, struct lws_cache_msg_struct *msg, int push);
/**********************************************************************************
  Function:      pnr_get_genralfriend_byindex
  Description:   根据用户id查找用户的好友中非本地好友
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
    
int pnr_get_genralfriend_byindex(int index,struct pnr_usermapping_struct* pulist,int distinct)
{
    int i = 0,j = 0;
    struct im_friends_struct* pfriend = NULL;
    if(index <= 0 || index > PNR_IMUSER_MAXNUM || pulist == NULL)
    {
        return ERROR;
    }
    memset(pulist,0,sizeof(struct pnr_usermapping_struct));
    pfriend = &(g_imusr_array.usrnode[index].friends[0]);
    for(i=0;i<PNR_IMUSER_FRIENDS_MAXNUM;i++)
    {
        if(pfriend[i].exsit_flag == TRUE)
        {
            if(distinct == TRUE)
            {
                for(j=0;j<pulist->user_num;j++)
                {
                }
            }
        }
    }
    return OK;
}

/**********************************************************************************
  Function:      pnr_groupinfo_init
  Description:   群group信息初始化
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int pnr_groupinfo_init(void)
{
    int i =0,gid = 0,uid = 0;
    char **dbResult; 
    char *errmsg;
    int nRow, nColumn;
    char sql_cmd[SQL_CMD_LEN] = {0};
    int offset=0;
    char group_path[PNR_FILEPATH_MAXLEN] = {0};

    snprintf(group_path,PNR_FILEPATH_MAXLEN,"%s%s",DAEMON_PNR_USERDATA_DIR,PNR_GROUP_DATA_PATH);
    if(access(group_path,F_OK) != OK)
    {
        snprintf(sql_cmd,SQL_CMD_LEN,"mkdir -p %s",group_path);
        system(sql_cmd);   
    }
    for(i=0;i<PNR_GROUP_MAXNUM;i++)
    {
        pthread_mutex_init(&(g_grouplock[i]),NULL);
        snprintf(group_path,PNR_FILEPATH_MAXLEN,"%s%sg%d",DAEMON_PNR_USERDATA_DIR,PNR_GROUP_DATA_PATH,i);
        if(access(group_path,F_OK) != OK)
        {
            snprintf(sql_cmd,SQL_CMD_LEN,"mkdir -p %s",group_path);
            system(sql_cmd);   
        }
    }
    //初始化群组列表
    //grouplist_tbl(id,hash,owner,ownerid,verify,manager,gname,createtime);
    memset(&g_grouplist,0,(PNR_GROUP_MAXNUM+1)*sizeof(struct group_info));
    snprintf(sql_cmd, SQL_CMD_LEN, "select id,hash,owner,ownerid,verify,manager,gname from grouplist_tbl order by id;");
    pthread_mutex_lock(&g_grouptoplock);
    if(sqlite3_get_table(g_groupdb_handle, sql_cmd, &dbResult, &nRow, 
               &nColumn, &errmsg) == SQLITE_OK)
    {
        offset = nColumn; //字段值从offset开始呀
        for(i = 0; i < nRow ; i++ )
        {
            gid = atoi(dbResult[offset]);
            if(gid >=0 && gid < PNR_GROUP_MAXNUM)
            {
                g_groupnum++;
                g_grouplist[gid].init_flag = TRUE;
                snprintf(g_grouplist[gid].group_filepath,PNR_FILEPATH_MAXLEN,"%s%sg%d",DAEMON_PNR_USERDATA_DIR,PNR_GROUP_DATA_PATH,gid);
                if(dbResult[offset+1] != NULL)
                {
                    strcpy(g_grouplist[gid].group_hid,dbResult[offset+1]);
                }
                if(dbResult[offset+2] != NULL)
                {
                    strcpy(g_grouplist[gid].owner,dbResult[offset+2]);
                }
                g_grouplist[gid].ownerid = atoi(dbResult[offset+3]);
                g_grouplist[gid].verify = atoi(dbResult[offset+4]);
                if(dbResult[offset+5] != NULL)
                {
                    //暂时没有manager
                }
                if(dbResult[offset+6] != NULL)
                {
                    strcpy(g_grouplist[gid].group_name,dbResult[offset+6]);
                }
            }
            offset += nColumn;
        }
        sqlite3_free_table(dbResult);
    }
    pthread_mutex_unlock(&g_grouptoplock);
    for(gid =0;gid<PNR_GROUP_MAXNUM;gid++)
    {
        pthread_mutex_lock(&g_grouplock[gid]);
        if(g_grouplist[gid].init_flag == TRUE)
        {
            //groupuser_tbl(gid,uid,uindex,type,initmsgid,lastmsgid,timestamp,utoxid,uname,uremark,gremark,pubkey)
            snprintf(sql_cmd, SQL_CMD_LEN, "select uid,uindex,type,initmsgid,lastmsgid,utoxid,uname,uremark,gremark,pubkey from groupuser_tbl where gid=%d order by uid;",gid);
            if(sqlite3_get_table(g_groupdb_handle, sql_cmd, &dbResult, &nRow, 
                           &nColumn, &errmsg) == SQLITE_OK)
            {
                offset = nColumn; //字段值从offset开始呀
                for(i = 0; i < nRow ; i++ )
                {
                    uid = atoi(dbResult[offset+1]);
                    if(uid >= 0 && uid <= PNR_IMUSER_MAXNUM)
                    {
                        g_grouplist[gid].user_num++;
                        memset(&g_grouplist[gid].user[uid],0,sizeof(struct group_user));
                        g_grouplist[gid].user[uid].gindex =  atoi(dbResult[offset]);
                        g_grouplist[gid].user[uid].userindex = uid;
                        g_grouplist[gid].user[uid].type = atoi(dbResult[offset+2]);
                        g_grouplist[gid].user[uid].init_msgid = atoi(dbResult[offset+3]);
                        g_grouplist[gid].user[uid].last_msgid = atoi(dbResult[offset+4]);
                        if(dbResult[offset+5] != NULL)
                        {
                            strcpy(g_grouplist[gid].user[uid].toxid,dbResult[offset+5]);
                        }
                        if(dbResult[offset+6] != NULL)
                        {
                            strcpy(g_grouplist[gid].user[uid].username,dbResult[offset+6]);
                        }
                        if(dbResult[offset+7] != NULL)
                        {
                            strcpy(g_grouplist[gid].user[uid].user_remarks,dbResult[offset+7]);
                        }
                        if(dbResult[offset+8] != NULL)
                        {
                            strcpy(g_grouplist[gid].user[uid].group_remarks,dbResult[offset+8]);
                        } 
                        if(dbResult[offset+9] != NULL)
                        {
                            strcpy(g_grouplist[gid].user[uid].userkey,dbResult[offset+9]);
                        } 
                        strcpy(g_grouplist[gid].user[uid].user_pubkey,g_imusr_array.usrnode[uid].user_pubkey);
                    }
                    offset += nColumn;
                }
                sqlite3_free_table(dbResult);
            }
            pnr_groupmsg_dbget_lastmsgid(gid,&g_grouplist[gid].last_msgid);
        }
        pthread_mutex_unlock(&g_grouplock[gid]);
    }
    return OK;
}
/*****************************************************************************
 函 数 名  : pnr_gettoxid_byhashid
 功能描述  : 根据hashid转化toxid
 输入参数  : char *path  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月23日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_gettoxid_byhashid(char* hash_id,char* tox_id)
{
    int u_index,f_num;
    unsigned int tox_hashnum = 0;
    char index_str[PNR_INDEX_HASHSTR_LEN+1] = {0};
    char hash_str[PNR_BKDR_HASHSTR_LEN+1] = {0};
    if(hash_id == NULL || tox_id == NULL)
    {
        return ERROR;
    }
    strncpy(index_str,hash_id,PNR_INDEX_HASHSTR_LEN);
    u_index = atoi(index_str);
    memset(index_str,0,sizeof(index_str));
    strncpy(index_str,hash_id+PNR_INDEX_HASHSTR_LEN,PNR_INDEX_HASHSTR_LEN);
    f_num = atoi(index_str);
    if(u_index > PNR_IMUSER_MAXNUM || f_num > PNR_IMUSER_FRIENDS_MAXNUM)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_gettoxid_byhashid:hash_id(%s) err",hash_id);
        return ERROR;
    }
    strncpy(hash_str,hash_id+PNR_INDEX_HASHSTR_LEN+PNR_INDEX_HASHSTR_LEN,PNR_BKDR_HASHSTR_LEN);
    tox_hashnum = (unsigned int)pnr_htoi(hash_str);
    if(f_num == 0)
    {
        if(g_imusr_array.usrnode[u_index].hashid != tox_hashnum)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_gettoxid_byhashid:user(%d) hashnum(%d:%d)err",
                u_index,g_imusr_array.usrnode[u_index].hashid,tox_hashnum);
            return ERROR;
        }
        strcpy(tox_id,g_imusr_array.usrnode[u_index].user_toxid);
    }
    else
    {
        if(g_imusr_array.usrnode[u_index].friends[f_num-1].hashid != tox_hashnum)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_gettoxid_byhashid:user(%d) hashnum(%d:%d)err",
                u_index,g_imusr_array.usrnode[u_index].friends[f_num-1].hashid,tox_hashnum);
            return ERROR;
        }
        strcpy(tox_id,g_imusr_array.usrnode[u_index].friends[f_num-1].user_toxid);
    }
    return OK;
}
/*****************************************************************************
 函 数 名  : pnr_checkrepeat_bymsgid
 功能描述  : 根据msgid检测该消息是否是重复消息
 输入参数  : index,msgid  
 输出参数  : repeat_flag
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月23日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_checkrepeat_bymsgid(int index,long msgid,int* repeat_flag)
{
    long last_cachemsgid = 0;
    int i = 0,last_cacheid = 0;
    if(index <0 || index >= PNR_IMUSER_MAXNUM || msgid == 0)
    {
        return FALSE;
    }
    last_cacheid = g_imusr_array.usrnode[index].lastmsgid_index;
    if(last_cacheid < 0 || last_cacheid >= IM_MSGID_CACHENUM)
    {
        return FALSE;
    }
    last_cachemsgid = g_imusr_array.usrnode[index].cache_msgid[last_cacheid];
    if(msgid == last_cachemsgid)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_checkrepeat_bymsgid:user(%d) rec repeatmsg(%ld)",index,msgid);
        *repeat_flag = TRUE;
        return TRUE;
    }
    else if(msgid < last_cachemsgid)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_checkrepeat_bymsgid:user(%d) rec repeatmsg(%ld) lastcacheid(%ld)",index,msgid,last_cachemsgid);
        for(i=0;i<IM_MSGID_CACHENUM;i++)
        {
            if(msgid == g_imusr_array.usrnode[index].cache_msgid[i])
            {
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_checkrepeat_bymsgid:user(%d) rec repeatmsg(%ld)",index,msgid);
                *repeat_flag = TRUE;
                return TRUE;
            }
        }
    }
    //把新的缓存id入队列
    last_cacheid = ((last_cacheid+1)% IM_MSGID_CACHENUM);
    g_imusr_array.usrnode[index].cache_msgid[last_cacheid] = msgid;
    g_imusr_array.usrnode[index].lastmsgid_index = last_cacheid;
    return FALSE;
}

/*****************************************************************************
 函 数 名  : pnr_check_update_devinfo_bytoxid
 功能描述  : 检查并更新用户所属dev信息
 输入参数  : char *path  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月23日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_check_update_devinfo_bytoxid(int index,char* ftox_id,char* dev_id,char* dev_name)
{
    int f_id = 0;
    struct im_friends_struct* p_friend = NULL;
    if(index <=0 || index > PNR_IMUSER_MAXNUM || ftox_id == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_check_update_devinfo_bytoxid:bad index(%d)",index);
        return ERROR;
    }

    if(strlen(dev_id) != TOX_ID_STR_LEN || strlen(dev_name) == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_check_update_devinfo_bytoxid:bad dev_id(%s)",dev_id);
        return ERROR;
    }
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_check_update_devinfo_bytoxid:indxe(%d) f_user(%s) devid(%s) devname(%s)",
        index,ftox_id,dev_id,dev_name);

    //更新系统内存中的
    f_id = get_friendid_bytoxid(index,ftox_id);
    if(f_id >= 0)
    {
        p_friend = &g_imusr_array.usrnode[index].friends[f_id];
        if(strcmp(p_friend->user_devid,dev_id) != OK)
        {
            memset(p_friend->user_devid,0,TOX_ID_STR_LEN);
            strcpy(p_friend->user_devid,dev_id);
        }
        if(strcmp(p_friend->user_devname,dev_name) != OK)
        {
            memset(p_friend->user_devname,0,PNR_USERNAME_MAXLEN);
            strcpy(p_friend->user_devname,dev_name);
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_check_update_devinfo_bytoxid:get user(%d) f_id(%d) dev(%s:%s)",index,f_id,dev_id,dev_name);
    }
    if(pnr_userdev_mapping_dbupdate(ftox_id,dev_id,dev_name) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_userdev_mapping_dbupdate failed");
        return ERROR;
    }
    return OK;
}
/*****************************************************************************
 函 数 名  : pnr_addfriend_devinfo_bytoxid
 功能描述  : 添加好友的设备信息
 输入参数  : char *path  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月23日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_addfriend_devinfo_bytoxid(int index,char* ftox_id)
{
    int f_id = 0;
    struct im_friends_struct* p_friend = NULL;
    struct im_userdev_mapping_struct devinfo;
    if(index <=0 || index > PNR_IMUSER_MAXNUM || ftox_id == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_addfriend_devinfo_bytoxid:bad index(%d)",index);
        return ERROR;
    }
    memset(&devinfo,0,sizeof(devinfo));
    strcpy(devinfo.user_toxid,ftox_id);
    if(pnr_usrdev_mappinginfo_sqlget(&devinfo) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_addfriend_devinfo_bytoxid:pnr_usrdev_mappinginfo_sqlget failed");
        return ERROR;
    }

    //更新系统内存中的
    f_id = get_friendid_bytoxid(index,ftox_id);
    if(f_id >= 0)
    {
        p_friend = &g_imusr_array.usrnode[index].friends[f_id];
        if(strcmp(p_friend->user_devid,devinfo.user_devid) != OK)
        {
            memset(p_friend->user_devid,0,TOX_ID_STR_LEN);
            strcpy(p_friend->user_devid,devinfo.user_devid);
        }
        if(strcmp(p_friend->user_devname,devinfo.user_devname) != OK)
        {
            memset(p_friend->user_devname,0,PNR_USERNAME_MAXLEN);
            strcpy(p_friend->user_devname,devinfo.user_devname);
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_addfriend_devinfo_bytoxid:get user(%d) f_id(%d) dev(%s:%s)",
            index,f_id,devinfo.user_devid,devinfo.user_devname);
    }
    return OK;
}

/*****************************************************************************
 函 数 名  : pnr_autoadd_localfriend
 功能描述  : 自动添加本地好友
 输入参数  : char *path  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月23日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_autoadd_localfriend(int index,int f_index,struct pnr_account_struct* p_srcaccount)
{
    struct im_user_struct* p_self = NULL;
    struct im_user_struct* p_friend = NULL;
    struct pnr_account_struct friend_account;
    if(f_index <= 0 || f_index > PNR_IMUSER_MAXNUM ||
        index <= 0 || index > PNR_IMUSER_MAXNUM ||p_srcaccount == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_autoadd_localfriend:bad index(%d) f_index(%d)",index,f_index);
        return ERROR;
    }
    if(p_srcaccount->active == FALSE)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_autoadd_localfriend:p_srcaccount not active");
        return ERROR;
    }
    if(index == f_index)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_autoadd_localfriend:get index (%d) err",index);
        return ERROR;
    }
    p_self = &g_imusr_array.usrnode[index];
    p_friend = &g_imusr_array.usrnode[f_index];
    memset(&friend_account,0,sizeof(friend_account));
    strcpy(friend_account.toxid,p_friend->user_toxid);
    pnr_account_dbget_byuserid(&friend_account);
    if(friend_account.active == FALSE)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_autoadd_localfriend:friend_account not active");
        return ERROR;
    }
    pnr_friend_dbinsert(p_self->user_toxid,p_friend->user_toxid,friend_account.nickname,friend_account.user_pubkey);
    im_nodelist_addfriend(index,p_self->user_toxid,p_friend->user_toxid,friend_account.nickname,friend_account.user_pubkey);
    pnr_check_update_devinfo_bytoxid(index,p_friend->user_toxid,g_daemon_tox.user_toxid,g_dev_nickname);
    pnr_friend_dbinsert(p_friend->user_toxid,p_self->user_toxid,p_srcaccount->nickname,p_srcaccount->user_pubkey);
    im_nodelist_addfriend(f_index,p_friend->user_toxid,p_self->user_toxid,p_srcaccount->nickname,p_srcaccount->user_pubkey);
    pnr_check_update_devinfo_bytoxid(f_index,p_self->user_toxid,g_daemon_tox.user_toxid,g_dev_nickname);
    return OK;
}

/*****************************************************************************
 函 数 名  : im_get_file_size
 功能描述  : 获取文件大小
 输入参数  : char *path  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月23日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_get_file_size(char *path)
{
    struct stat fstat;
    int ret = 0;
    int size = 0;
    
    ret = stat(path, &fstat);
    if (ret < 0) {
        size = 0;
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get file stat err(%s-%d)", path, errno);
    } else {
        size = fstat.st_size;
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "get file size(%s-%d)", path, size);
    }

    return size;
}

/*****************************************************************************
 函 数 名  : __minimal_destroy_message
 功能描述  : 销毁消息
 输入参数  : void *_msg  
 输出参数  : 无
 返 回 值  : static
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月23日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
static void __minimal_destroy_message(void *_msg)
{
	struct msg *msg = _msg;

	free(msg->payload);
	msg->payload = NULL;
	msg->len = 0;
}
/*****************************************************************************
 函 数 名  : callback_minimal
 功能描述  : 接收消息入口
 输入参数  : struct lws *wsi                   
             enum lws_callback_reasons reason  
             void *user                        
             void *in                          
             size_t len                        
 输出参数  : 无
 返 回 值  : static
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年9月28日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
static int callback_minimal(struct lws *wsi, enum lws_callback_reasons reason,
			void *user, void *in, size_t len)
{
	struct per_session_data__minimal *pss =
			(struct per_session_data__minimal *)user;
	struct per_vhost_data__minimal *vhd =
			(struct per_vhost_data__minimal *)
			lws_protocol_vh_priv_get(lws_get_vhost(wsi),
					lws_get_protocol(wsi));
	int m;
    int retmsg_len = 0;
    int ret_flag = FALSE;
	const struct msg *pmsg;
	struct msg amsg;
	int n;
	
	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
				lws_get_protocol(wsi),
				sizeof(struct per_vhost_data__minimal));
		if (!vhd)
			return 1;

		vhd->context = lws_get_context(wsi);
		vhd->protocol = lws_get_protocol(wsi);
		vhd->vhost = lws_get_vhost(wsi);
		
		break;

	case LWS_CALLBACK_ESTABLISHED:
		pss->ring = lws_ring_create(sizeof(struct msg), 64,
					    __minimal_destroy_message);
		if (!pss->ring) {
			DEBUG_PRINT(DEBUG_LEVEL_ERROR,"%s: failed to create ring", __func__);
			return 1;
		}
		
		/* add ourselves to the list of live pss held in the vhd */
		lws_ll_fwd_insert(pss, pss_list, vhd->pss_list);
		pss->wsi = wsi;

		pthread_mutex_lock(&pss->lock_ring);
		pss->tail = lws_ring_get_oldest_tail(pss->ring);
		pthread_mutex_unlock(&pss->lock_ring);
		
		break;

	case LWS_CALLBACK_CLOSED:
        if (pss->user_index != 0) {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"#####socket(%d) wsi(%p)close#####",pss->user_index,wsi);
            g_lws_handler[pss->user_index] = NULL;
            g_imusr_array.usrnode[pss->user_index].pss = NULL;
			g_imusr_array.usrnode[pss->user_index].user_online_type = USER_ONLINE_TYPE_NONE;
        }
        /* remove our closing pss from the list of live pss */
		lws_ll_fwd_remove(struct per_session_data__minimal, pss_list,
				  pss, vhd->pss_list);

		pthread_mutex_lock(&pss->lock_ring);
		lws_ring_destroy(pss->ring);
		pthread_mutex_unlock(&pss->lock_ring);
		//DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d) remove end",pss->user_index);
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
        /*if (lws_send_onemsg(pss->user_index, wsi, &ret_flag) != OK)
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"lws_send_onemsg failed");

		if(!list_empty(&g_lws_msglist[pss->user_index].list)) {
			lws_callback_on_writable(pss->wsi);
			break;
		}*/

		pthread_mutex_lock(&pss->lock_ring);
		pmsg = lws_ring_get_element(pss->ring, &pss->tail);
		if (!pmsg) {
			pthread_mutex_unlock(&pss->lock_ring);
			break;
		}

		/* notice we allowed for LWS_PRE in the payload already */
		m = lws_write(wsi, pmsg->payload + LWS_PRE, pmsg->len, LWS_WRITE_TEXT);
		if (m < (int)pmsg->len) {
			DEBUG_PRINT(DEBUG_LEVEL_ERROR,"ERROR %d writing to ws socket", m);
			pthread_mutex_unlock(&pss->lock_ring);
			return -1;
		}
		
        DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"send user(%d) msg(%s)",pss->user_index,(pmsg->payload + LWS_PRE));

		lws_ring_consume_single_tail(pss->ring, &pss->tail, 1);

		/* more to do? */
		if (lws_ring_get_element(pss->ring, &pss->tail))
			/* come back as soon as we can write more */
			lws_callback_on_writable(pss->wsi);

		pthread_mutex_unlock(&pss->lock_ring);
		break;

	case LWS_CALLBACK_RECEIVE:
        if (im_rcvmsg_deal(pss, in, len, pss->msgretbuf,
			&retmsg_len, &ret_flag, &pss->user_index) != OK) {
			DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_rcvmsg_deal failed");
            snprintf(pss->msgretbuf,1024,IMSERVER_BAD_MSG_RET);
            retmsg_len = IMSERVER_BAD_MSG_RETLEN;
        }
		
        if (pss->user_index > 0 && pss->user_index <= PNR_IMUSER_MAXNUM) {
            g_lws_handler[pss->user_index] = wsi;
            g_imusr_array.usrnode[pss->user_index].pss = pss;
        }

        if (ret_flag == FALSE) {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"no need ret");
            break;
        } else {
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"id(%d) ret msg(%d):(%s)",pss->user_index,retmsg_len,pss->msgretbuf);
		}

		pthread_mutex_lock(&pss->lock_ring);
		
		/* only create if space in ringbuffer */
		n = (int)lws_ring_get_count_free_elements(pss->ring);
		if (!n) {
			DEBUG_PRINT(DEBUG_LEVEL_ERROR, "dropping!");
			pthread_mutex_unlock(&pss->lock_ring);
			break;
		}
		
		amsg.payload = malloc(LWS_PRE + retmsg_len + 1);
		if (!amsg.payload) {
			DEBUG_PRINT(DEBUG_LEVEL_ERROR, "OOM: dropping malloc(%d)",retmsg_len);
			pthread_mutex_unlock(&pss->lock_ring);
			break;
		}
		
        memset(amsg.payload, 0, LWS_PRE + retmsg_len + 1);
		strncpy((char *)amsg.payload + LWS_PRE, pss->msgretbuf, retmsg_len);
		amsg.len = retmsg_len;
        //DEBUG_PRINT(DEBUG_LEVEL_INFO, "copy(%d:%s)!",retmsg_len,(amsg.payload + LWS_PRE));
		n = lws_ring_insert(pss->ring, &amsg, 1);
		if (n != 1) {
			__minimal_destroy_message(&amsg);
			DEBUG_PRINT(DEBUG_LEVEL_ERROR, "dropping!");
			pthread_mutex_unlock(&pss->lock_ring);
			break;
		}

		pthread_mutex_unlock(&pss->lock_ring);
        lws_callback_on_writable(wsi);
		break;

	default:
		break;
	}

	return 0;
}

/*****************************************************************************
 函 数 名  : callback_pnr_bin
 功能描述  : 处理二进制消息
 输入参数  : struct lws *wsi                   
             enum lws_callback_reasons reason  
             void *user                        
             void *in                          
             size_t len                        
 输出参数  : 无
 返 回 值  : static
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月8日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
static int callback_pnr_bin(struct lws *wsi, enum lws_callback_reasons reason,
			void *user, void *in, size_t len)
{
	struct per_session_data__minimal_bin *pss =
			(struct per_session_data__minimal_bin *)user;
	struct per_vhost_data__minimal_bin *vhd =
			(struct per_vhost_data__minimal_bin *)
			lws_protocol_vh_priv_get(lws_get_vhost(wsi),
					lws_get_protocol(wsi));
	int m;
    int retmsg_len = 0;
    int ret_flag = FALSE;
	const struct msg *pmsg;
	struct msg amsg;
	int n, ret;
	
	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
				lws_get_protocol(wsi),
				sizeof(struct per_vhost_data__minimal_bin));
		if (!vhd)
			return 1;

		vhd->context = lws_get_context(wsi);
		vhd->protocol = lws_get_protocol(wsi);
		vhd->vhost = lws_get_vhost(wsi);
		
		break;

	case LWS_CALLBACK_ESTABLISHED:
		pss->ring = lws_ring_create(sizeof(struct msg), 64,
					    __minimal_destroy_message);
		if (!pss->ring) {
			lwsl_err("%s: failed to create ring\n", __func__);
			return 1;
		}
		
		/* add ourselves to the list of live pss held in the vhd */
		lws_ll_fwd_insert(pss, pss_list, vhd->pss_list);
		pss->wsi = wsi;
		pss->tail = lws_ring_get_oldest_tail(pss->ring);
		
		break;

	case LWS_CALLBACK_CLOSED:
        if (pss->user_index != 0) {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"#####bin socket(%d) wsi(%p) close#####",pss->user_index,wsi);
        }
		
        /* remove our closing pss from the list of live pss */
		lws_ll_fwd_remove(struct per_session_data__minimal_bin, pss_list,
				  pss, vhd->pss_list);

		lws_ring_destroy(pss->ring);
			
		if (pss->fd) {
			close(pss->fd);
			pss->fd = 0;
		}
		
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		pmsg = lws_ring_get_element(pss->ring, &pss->tail);
		if (!pmsg) {
			break;
		}

		/* notice we allowed for LWS_PRE in the payload already */
		m = lws_write(wsi, pmsg->payload + LWS_PRE, pmsg->len, LWS_WRITE_BINARY);
		if (m < (int)pmsg->len) {
			lwsl_err("ERROR %d writing to ws socket\n", m);
			return -1;
		}
		
		lws_ring_consume_single_tail(pss->ring, &pss->tail, 1);

		/* more to do? */
		if (lws_ring_get_element(pss->ring, &pss->tail))
			/* come back as soon as we can write more */
			lws_callback_on_writable(pss->wsi);
		
		break;

	case LWS_CALLBACK_RECEIVE:
        //DEBUG_PRINT(DEBUG_LEVEL_INFO, "rec msg(%d) wsi(%p)", len,wsi);
		ret = im_rcvmsg_deal_bin(pss, in, len, pss->msgretbuf,
			&retmsg_len, &ret_flag, &pss->user_index);
        if (ret == ERROR) {
			DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_rcvmsg_deal_bin failed");
            snprintf(pss->msgretbuf, 1024, IMSERVER_BAD_MSG_RET);
            retmsg_len = IMSERVER_BAD_MSG_RETLEN;
			ret_flag = FALSE;
        } else if (ret == 2) {
			break;
		}
		
        if (ret_flag == FALSE) {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_rcvmsg_deal:no need ret");
            break;
        } else {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"id(%d) ret msg(%d)",pss->user_index,retmsg_len);
		}

		/* only create if space in ringbuffer */
		n = (int)lws_ring_get_count_free_elements(pss->ring);
		if (!n) {
			DEBUG_PRINT(DEBUG_LEVEL_ERROR,"dropping!");
			break;
		}

		amsg.payload = malloc(LWS_PRE + retmsg_len+1);
		if (!amsg.payload) {
			DEBUG_PRINT(DEBUG_LEVEL_ERROR,"OOM: dropping malloc(%d)",retmsg_len);
			break;
		}
        memset(amsg.payload,0,(LWS_PRE + retmsg_len+1));
		memcpy((char *)amsg.payload + LWS_PRE, pss->msgretbuf, retmsg_len);
		amsg.len = retmsg_len;
		n = lws_ring_insert(pss->ring, &amsg, 1);
		if (n != 1) {
			__minimal_destroy_message(&amsg);
			DEBUG_PRINT(DEBUG_LEVEL_ERROR,"dropping!");
		}

        lws_callback_on_writable(wsi);
		break;

	default:
		break;
	}

	return 0;
}

/**********************************************************************************
  Function:      get_rnodeidbytoxid
  Description:  根据toxid获取根节点id
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:没找到
                 num:实例id
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int get_rnodefidbytoxid(char* p_toxid)
{
    int i =0;
    if(p_toxid == NULL || strlen(p_toxid) != TOX_ID_STR_LEN)
    {
        return -1;
    }
    for(i=1;i<=PNR_IMUSER_MAXNUM;i++)
    {
        if(strcmp(p_toxid,g_rnode[i].tox_id) == OK)
        {
            return g_rnode[i].f_id;
        }
    }
    return 0xFF;
}
/**********************************************************************************
  Function:      get_indexbytoxid
  Description:  根据toxid获取实例id
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:没找到
                 num:实例id
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int get_indexbytoxid(char* p_toxid)
{
    int i =0;
    if(p_toxid == NULL || strlen(p_toxid) != TOX_ID_STR_LEN)
    {
        return 0;
    }
    for(i=1;i<=PNR_IMUSER_MAXNUM;i++)
    {
        if(strcmp(p_toxid,g_imusr_array.usrnode[i].user_toxid) == OK)
        {
            return i;
        }
    }
    return 0;
}
/**********************************************************************************
  Function:      get_gidbygrouphid
  Description:  根据group hashid获取群id
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        -1:没找到
                 num:实例id
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int get_gidbygrouphid(char* p_ghashid)
{
    int i =0;
    if(p_ghashid == NULL || strlen(p_ghashid) != TOX_ID_STR_LEN)
    {
        return -1;
    }
    for(i=0;i<=PNR_GROUP_MAXNUM;i++)
    {
        if(strcmp(p_ghashid,g_grouplist[i].group_hid) == OK)
        {
            return i;
        }
    }
    return -1;
}
/**********************************************************************************
  Function:      get_guserbytoxid
  Description:  根据toxid获取该用户在特定群里的用户
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        -1:没找到
                 num:实例id
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
struct group_user* get_guserbytoxid(int gid,char* userid)
{
    int i =0;
    if(userid == NULL || gid < 0 || gid > PNR_GROUP_MAXNUM)
    {
        return NULL;
    }
    for(i=0;i<=PNR_GROUP_USER_MAXNUM;i++)
    {
        if(strcmp(userid,g_grouplist[gid].user[i].toxid) == OK)
        {
            return &g_grouplist[gid].user[i];
        }
    }
    return NULL;
}
/**********************************************************************************
  Function:      get_guserbyuindex
  Description:  根据uindex获取该用户在特定群里的用户
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        -1:没找到
                 num:实例id
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
struct group_user* get_guserbyuindex(int gid,int uindex)
{
    int i =0;
    if(uindex <=0 || uindex > PNR_IMUSER_MAXNUM || gid < 0 || gid > PNR_GROUP_MAXNUM)
    {
        return NULL;
    }
    for(i=0;i<=PNR_GROUP_USER_MAXNUM;i++)
    {
        if(uindex == g_grouplist[gid].user[i].userindex)
        {
            return &g_grouplist[gid].user[i];
        }
    }
    return NULL;
}
/**********************************************************************************
  Function:      pnr_account_gettype_byusn
  Description:  根据usn获取账户类型
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:没找到
                 num:实例id
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int pnr_account_gettype_byusn(char* p_usn,int* p_usertype)
{
    char usertype_buff[PNR_USN_USERTYPE_LEN+1] = {0};
    if(p_usn == NULL)
    {
        return ERROR;
    }
    strncpy(usertype_buff,p_usn,PNR_USN_USERTYPE_LEN);
    *p_usertype = atoi(usertype_buff);
    return OK;
}

/**********************************************************************************
  Function:      insert_lws_msgnode
  Description:   插入一个lws消息节点
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int insert_lws_msgnode(int id,char* puser,char* pmsg,int msg_len)
{
    struct lws_msg_struct* pnode = NULL;
	
    if(id <= 0 || id > PNR_IMUSER_MAXNUM || pmsg == NULL || puser == NULL)
    {
        return ERROR;
    }
	
    pnode = (struct lws_msg_struct*)malloc(sizeof(struct lws_msg_struct));
    if(pnode == NULL)
    {
        return ERROR;
    }
	
    pnode->msg_payload = (void*)malloc(msg_len + LWS_PRE+1);
    if(pnode->msg_payload == NULL)
    {
        free(pnode);
        return ERROR;
    }
	
    pnode->index = id;
    pnode->msg_len = msg_len;
    strcpy(pnode->user_id, puser);
    memset((char*)pnode->msg_payload,0,msg_len + LWS_PRE+1);
    strncpy((char*)pnode->msg_payload + LWS_PRE,pmsg,msg_len);

	pthread_mutex_lock(&lws_msglock[id]);
    list_add_tail(&(pnode->list),&(g_lws_msglist[id].list));
    pthread_mutex_unlock(&lws_msglock[id]);

	if(g_lws_handler[id] != NULL)
    {
        lws_callback_on_writable(g_lws_handler[id]);
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"id(%d) (%p)lws_callback_on_writable",id,g_lws_handler[id]);
    }
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"insert_lws_msgnode(%d:%s) len(%d)",id,pmsg,msg_len);
    return OK;
}

/*****************************************************************************
 函 数 名  : insert_lws_msgnode_ring
 功能描述  : 添加待发送消息到环形队列中
 输入参数  : int id       
             char* pmsg   
             int msg_len  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月26日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int insert_lws_msgnode_ring(int id, char *pmsg, int msg_len)
{
    int n = 0;
    struct msg amsg;
    struct per_session_data__minimal *pss = g_imusr_array.usrnode[id].pss;

    if (!pss) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pss null!");
		return -1;
    }

	pthread_mutex_lock(&pss->lock_ring);
	
    n = (int)lws_ring_get_count_free_elements(pss->ring);
	if (!n) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "lws dropping!");
		pthread_mutex_unlock(&pss->lock_ring);
		return -1;
	}

	amsg.payload = malloc(LWS_PRE + msg_len+1);
	if (!amsg.payload) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "OOM: dropping");
		pthread_mutex_unlock(&pss->lock_ring);
		return -1;
	}
	
    memset(amsg.payload,0,(LWS_PRE + msg_len+1));
	strncpy((char *)amsg.payload + LWS_PRE, pmsg, msg_len);
	amsg.len = msg_len;
	n = lws_ring_insert(pss->ring, &amsg, 1);
	if (n != 1) {
		__minimal_destroy_message(&amsg);
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "dropping!");
		pthread_mutex_unlock(&pss->lock_ring);
        return -1;
	}

	pthread_mutex_unlock(&pss->lock_ring);

    if (g_lws_handler[id]) {
        lws_callback_on_writable(g_lws_handler[id]);
    } else {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "wsi null!");
        return -1;
    }

    return 0;
}

/**********************************************************************************
  Function:      lws_send_onemsg
  Description:   发送一个lws消息队列中的消息
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int lws_send_onemsg(int id,struct lws *wsi,int* break_flag)
{
    struct lws_msg_struct* pnode = NULL;
    int msglen = 0;

    if(id <= 0 || id > PNR_IMUSER_MAXNUM || wsi == NULL)
    {
        return OK;
    }

    if(list_empty(&g_lws_msglist[id].list) == TRUE)
    {
        return OK;
    }

    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"id(%d) not empty",id);
    pthread_mutex_lock (&lws_msglock[id]);
    pnode = list_first_entry(&g_lws_msglist[id].list, struct lws_msg_struct, list);
    list_del(&(pnode->list));
    pthread_mutex_unlock (&lws_msglock[id]);

    if(pnode == NULL)
    {
        return ERROR;
    }

    if(pnode->msg_payload == NULL)
    {
        free(pnode);
        return ERROR;
    }

    if(strcmp(pnode->user_id,g_imusr_array.usrnode[id].user_toxid) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"targetuser(%s) but cur_user(%s)",
            pnode->user_id,g_imusr_array.usrnode[id].user_toxid);
        free(pnode->msg_payload);
		free(pnode);
        return ERROR;
    }

    msglen = lws_write(wsi, pnode->msg_payload+LWS_PRE, pnode->msg_len,LWS_WRITE_TEXT);
    if(msglen != pnode->msg_len)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"return(%d)",msglen);
    }

    //free(pnode->msg_payload);//这个lws中释放了
    free(pnode);
    *break_flag = TRUE;
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"id(%d) send ok",id);
    return OK;
}

/*****************************************************************************
 函 数 名  : im_get_friend_info
 功能描述  : 获取好友本地id以及tox id
 输入参数  : int *friendid   
             int *friendnum  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月22日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_get_friend_info(int userid, char *to, int *friendid, int *friendnum)
{
	*friendnum = check_and_add_friends(g_tox_linknode[userid], to, 
		g_imusr_array.usrnode[userid].userinfo_fullurl);
	if (*friendnum < 0) {
		return ERROR;
	}

    return OK;
}

/*****************************************************************************
 函 数 名  : im_update_friend_nickname
 功能描述  : 更新好友昵称
 输入参数  : char* user_toxid  
             char *friend_toxid
             char* nickname
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月22日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_update_friend_nickname(char* user_toxid, char *friend_toxid, char* nickname)
{    
    int uid = 0, fid = 0;
    if(user_toxid == NULL || friend_toxid == NULL || nickname == NULL)
    {
        return ERROR;
    }
    uid = get_indexbytoxid(user_toxid);
    if(uid == 0)
    {
        return ERROR;
    }
    for(fid  = 0;fid  < PNR_IMUSER_FRIENDS_MAXNUM; fid++)
    {
        //遍历找到对应的好友
        if((g_imusr_array.usrnode[uid].friends[fid].exsit_flag == TRUE)
            &&(strcasecmp(g_imusr_array.usrnode[uid].friends[fid].user_toxid,friend_toxid) == OK))
        {
            memset(g_imusr_array.usrnode[uid].friends[fid].user_nickname,0,PNR_USERNAME_MAXLEN);
            strcpy(g_imusr_array.usrnode[uid].friends[fid].user_nickname,nickname);
            pnr_friend_dbupdate_nicename_bytoxid(user_toxid,friend_toxid,nickname);
            return OK;
        }
    }
    return ERROR;
}

/*****************************************************************************
 函 数 名  : im_msg_if_limited
 功能描述  : 判断消息是否限制重传次数
 输入参数  : int type  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月23日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_msg_if_limited(int type)
{
    switch (type) {
    case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
        return 1;

	default:
		return 0;
    }

    return 1;
}
char g_tox_sendmsg_cache[1500] = {0};
/*****************************************************************************
 函 数 名  : im_send_msg_deal
 功能描述  : 处理待发送消息
 输入参数  : 无
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月16日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
void im_send_msg_deal(int direction)
{
	int i = 0;
    int ret = 0;    
	struct lws_cache_msg_struct *msg = NULL;
    struct lws_cache_msg_struct *n = NULL;
	struct lws_cache_msg_struct *tmsg = NULL;
    struct lws_cache_msg_struct *tn = NULL;
    int node_num = 0;
    int msg_num = 0;
    int node_msgno = 0;
    int start_time = 0;
    int end_time = 0;
    //等待系统的p2p网络建立成功
    if(g_p2pnet_init_flag == FALSE)
    {
        return;
    }

	if (direction) {
		i = PNR_IMUSER_MAXNUM;
	} else {
		i = 0;
	}
    if(g_msglist_debugswitch == TRUE)
    {
        node_num = 0;
        msg_num = 0;
        node_msgno = 0;
        start_time = (int)time(NULL);
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_send_msg_deal:once start time(%d)",start_time);
    }
	while (1) {
		if (direction) {
			if (i-- == 0)
				break;
		} else {
			if (i++ == PNR_IMUSER_MAXNUM)
				break;
		}
		pthread_mutex_lock(&lws_cache_msglock[i]);
		if (!list_empty(&g_lws_cache_msglist[i].list)) {
            if(g_msglist_debugswitch == TRUE)
            {
               node_num++;
               msg_num += node_msgno;
               node_msgno = 0;
            }
			list_for_each_safe(msg, n, &g_lws_cache_msglist[i].list,struct lws_cache_msg_struct, list) 
            {
                if(g_msglist_debugswitch == TRUE)
                {
                    node_msgno++;
                }
				if (msg->resend == 0 || time(NULL) -  msg->timestamp > 3) {
					if (msg->resend == 0)
						msg->resend++;
					
					msg->timestamp = time(NULL);
                    //printf("user(%d) ctype(%d) cmd(%d) msg_len(%d)\n",msg->userid,msg->ctype,msg->type,msg->msglen);
                    switch (msg->ctype) {
                    case PNR_MSG_CACHE_TYPE_TOX:
						if (msg->resend > 50) {
							//DEBUG_PRINT(DEBUG_LEVEL_ERROR, "send msg failed!(user:%d:%s)", 
							//	msg->userid, msg->msg);
							//pnr_msgcache_dbdelete_nolock(msg);
							//continue;
						}
						
                        if (im_msg_if_limited(msg->type) && msg->resend++ > 3) {
                            pnr_msgcache_dbdelete_nolock(msg);
                            continue;
                        }

						msg->friendid = get_friendid_bytoxid(msg->userid, msg->toid);
						if (msg->friendid < 0 && msg->type != PNR_IM_CMDTYPE_DELFRIENDPUSH) {
							pnr_msgcache_dbdelete_nolock(msg);
							continue;
						}

						if (msg->type == PNR_IM_CMDTYPE_PUSHMSG) {
							pthread_mutex_lock(&g_imusr_array.usrnode[msg->userid].friends[msg->friendid].lock_sended);
							//get the first msg
							if (g_imusr_array.usrnode[msg->userid].friends[msg->friendid].sended == 0) {
								list_for_each_safe(tmsg, tn, &g_lws_cache_msglist[i].list, struct lws_cache_msg_struct, list) {
									if (tmsg->friendid == msg->friendid) {
										g_imusr_array.usrnode[msg->userid].friends[msg->friendid].sended = tmsg->msgid;
										break;
									}	
								}
							}

							if (g_imusr_array.usrnode[msg->userid].friends[msg->friendid].sended && 
								g_imusr_array.usrnode[msg->userid].friends[msg->friendid].sended != msg->msgid) {	
								pthread_mutex_unlock(&g_imusr_array.usrnode[msg->userid].friends[msg->friendid].lock_sended);
								continue;
							}
							pthread_mutex_unlock(&g_imusr_array.usrnode[msg->userid].friends[msg->friendid].lock_sended);
						}
						
						if (!g_tox_linknode[msg->userid])
                            continue;

 						if (msg->type != PNR_IM_CMDTYPE_DELFRIENDPUSH && !if_friend_available(msg->userid, msg->toid)) {
							pnr_msgcache_dbdelete_nolock(msg);
							continue;
						}
						
                        ret = im_get_friend_info(msg->userid, msg->toid, &msg->friendid, &msg->friendnum);
                        if (ret) {
                            continue;
						}

                    	ret = tox_friend_get_connection_status(g_tox_linknode[msg->userid], msg->friendnum, NULL);
                    	if (ret == TOX_CONNECTION_TCP || ret == TOX_CONNECTION_UDP) 
                        {
                            if(msg->msglen > MAX_CRYPTO_DATA_SIZE)
                            {
                                cJSON *RspJson = cJSON_Parse(msg->msg);
                				if (!RspJson) {
                					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "parse retbuf(%s) err!", msg->msg);
                                    continue;
                				}

                                cJSON *RspJsonParams = cJSON_GetObjectItem(RspJson, "data");
                				if (!RspJsonParams) {
                					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "parse data(%s) err!", msg->msg);
                					cJSON_Delete(RspJson);
                                    continue;
                				}

                				char *RspStrParams = RspJsonParams->valuestring;
                				if (!RspStrParams) 
                                {
                					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "print params(%s) err!", msg->msg);
                					cJSON_Delete(RspJson);
                                    continue;
                				}
                                struct tox_msg_send *tox_msg = (struct tox_msg_send *)calloc(1, sizeof(*tox_msg));
                                if(tox_msg == NULL)
                                {
                					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "alloc tox_msg failed");
                					cJSON_Delete(RspJson);
                                    continue;
                				}
                                //DEBUG_PRINT(DEBUG_LEVEL_INFO,"$$$get RspStrParams(%s)",RspStrParams);
                                memset(tox_msg,0,sizeof(struct tox_msg_send));
                                tox_msg->msg = RspStrParams;
                			    tox_msg->msgid = msg->msgid;
                			    tox_msg->friendnum = msg->friendnum;
                			    tox_msg->msglen = strlen(RspStrParams);
                                for(;tox_msg->offset < tox_msg->msglen;)
                                {
                                    cJSON *JsonFrame = cJSON_Duplicate(RspJson, true);
                    			    if (!JsonFrame) {
                    			        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "dup RspJson err!");
                    					break;
                    				}
                                    //去掉原始的data字段
                    				cJSON_DeleteItemFromObject(JsonFrame, "data");
                                    //填充新的字段
                                    strncpy(g_tox_sendmsg_cache, tox_msg->msg+tox_msg->offset, MAX_SEND_DATA_SIZE);
                                	cJSON_AddNumberToObject(JsonFrame, "offset", tox_msg->offset);
                                    tox_msg->offset += MAX_SEND_DATA_SIZE;
                                    if(tox_msg->offset >= tox_msg->msglen)
                                    {
                                        cJSON_AddNumberToObject(JsonFrame, "more", 0);
                                    }
                                    else
                                    {
                                        cJSON_AddNumberToObject(JsonFrame, "more", 1);
                                    }
                    				cJSON_AddStringToObject(JsonFrame, "data", g_tox_sendmsg_cache);
                    				char *RspStrSend = cJSON_PrintUnformatted_noescape(JsonFrame);
                    				if (!RspStrSend) {
                    					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "print RspJsonSend err!");
                    					cJSON_Delete(JsonFrame);
                    					break;
                    				}
                    				tox_friend_send_message(g_tox_linknode[msg->userid], msg->friendnum, TOX_MESSAGE_TYPE_NORMAL, 
                                		(uint8_t *)RspStrSend, strlen(RspStrSend), NULL);
                    				cJSON_Delete(JsonFrame);
                    				free(RspStrSend);
                                }
                                cJSON_Delete(RspJson);
                            }
                            else
                            {
                                tox_friend_send_message(g_tox_linknode[msg->userid], 
                                    msg->friendnum, TOX_MESSAGE_TYPE_NORMAL, 
                                    (uint8_t *)msg->msg, msg->msglen, NULL);
                            }
                            msg->resend++;
                        }
                        break;

                    case PNR_MSG_CACHE_TYPE_TOXF:
						if (msg->resend > 50) {
							//DEBUG_PRINT(DEBUG_LEVEL_ERROR, "send file failed!(user:%d:%s)", 
							//	msg->userid, msg->filename);
							//pnr_msgcache_dbdelete_nolock(msg);
							//continue;
						}
						
                        if (msg->filestatus) {
							//DEBUG_PRINT(DEBUG_LEVEL_ERROR, "sending file(%s)", msg->filename);
                            continue;
						}

						if (!g_tox_linknode[msg->userid]) {
							//DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get tox err(%d)", msg->userid);
							continue;
						}

                        ret = im_get_friend_info(msg->userid, msg->toid, &msg->friendid, &msg->friendnum);
                        if (ret) {
							DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get friendnum err(%d--%s)", msg->userid, msg->toid);
                            //这里加不上，就不要重试了
                            pnr_msgcache_dbdelete_nolock(msg);
                            continue;
						}

						ret = tox_friend_get_connection_status(g_tox_linknode[msg->userid], msg->friendnum, NULL);
                        if (ret == TOX_CONNECTION_TCP || ret == TOX_CONNECTION_UDP) {
	                    	msg->filestatus = 1;
	                    	im_send_file_by_tox(g_tox_linknode[msg->userid], msg, FALSE);
							msg->resend++;
                        } else {
							//DEBUG_PRINT(DEBUG_LEVEL_ERROR, "user(%d) friend(%s) offline", msg->userid, msg->toid);
						}
                        break;

                    case PNR_MSG_CACHE_TYPE_TOXAF:
                    case PNR_MSG_CACHE_TYPE_TOXA:
                    case PNR_MSG_CACHE_TYPE_LWS:
                        //DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d) ctype(%d) online_type(%d)",msg->userid,msg->ctype,g_imusr_array.usrnode[msg->userid].user_online_type);
                        if (g_imusr_array.usrnode[msg->userid].user_online_type == USER_ONLINE_TYPE_TOX 
                            && g_imusr_array.usrnode[msg->userid].appactive_flag == PNR_APPACTIVE_STATUS_FRONT) 
                        {
                            DEBUG_PRINT(DEBUG_LEVEL_INFO,"push user(%d)(%d:%s)",
                                g_imusr_array.usrnode[msg->userid].appid,msg->msglen,msg->msg);
                            tox_friend_send_message(qlinkNode, 
                                g_imusr_array.usrnode[msg->userid].appid, TOX_MESSAGE_TYPE_NORMAL, 
                                (uint8_t *)msg->msg, msg->msglen, NULL);
	                    }
						else if (g_imusr_array.usrnode[msg->userid].user_online_type == USER_ONLINE_TYPE_LWS
                            && g_imusr_array.usrnode[msg->userid].appactive_flag == PNR_APPACTIVE_STATUS_FRONT) 
	                    {
	                        insert_lws_msgnode_ring(msg->userid, msg->msg, msg->msglen);
	                    }
                        else
                        {
                            if(g_noticepost_enable == TRUE && msg->notice_flag == FALSE)
                            {
                                switch(msg->type)
                                {
                                    //case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
                                    //case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
                                    case PNR_IM_CMDTYPE_PUSHMSG:
                                    case PNR_IM_CMDTYPE_PUSHFILE:
                                    case PNR_IM_CMDTYPE_PUSHFILE_TOX: 
                                    case PNR_IM_CMDTYPE_GROUPMSGPUSH:
                                        //DEBUG_PRINT(DEBUG_LEVEL_INFO,"###user(%d) msg(%d) post_newmsg_notice###",msg->userid,msg->msgid);
                                        post_newmsg_notice(g_daemon_tox.user_toxid,
                                            g_imusr_array.usrnode[msg->userid].user_toxid,
                                            PNR_POSTMSG_PAYLOAD,TRUE);                                    
                                        break;
                                    default:
                                        break;
                                }
                                msg->notice_flag = TRUE;
                            }
							if (im_msg_if_limited(msg->type) && msg->resend++ > 3) {
								pnr_msgcache_dbdelete_nolock(msg);
								continue;
							}
                        }
						break;
					default:
						DEBUG_PRINT(DEBUG_LEVEL_ERROR, "wrong cache type(%d)", msg->ctype);
                    }
				}
			}
		}
		pthread_mutex_unlock(&lws_cache_msglock[i]);
	}
    if(g_msglist_debugswitch == TRUE)
    {
        end_time = (int)time(NULL);
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_send_msg_deal:once end time(%d),cost time(%d),node(%d) total_msg(%d)",
            end_time,(end_time-start_time),node_num,msg_num);
    }
}

/*****************************************************************************
 函 数 名  : im_nodelist_addfriend
 功能描述  : 添加好友节点
 输入参数  : int index        
             char* from_user  
             char* to_user    
             char* nickname   
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月10日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_nodelist_addfriend(int index,char* from_user,char* to_user,char* nickname,char* userkey)
{
    int i = 0,j = 0,none_index = 0,f_id = 0;
    //检查是否非法
    if(from_user == NULL || to_user == NULL || nickname == NULL || userkey == NULL)
    {
        return ERROR;
    }

    i = get_indexbytoxid(from_user);
    if(i != index)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%s) not found",from_user);
        return ERROR;
    }
    if(g_imusr_array.usrnode[i].friendnum >= PNR_IMUSER_FRIENDS_MAXNUM)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%s) friend full",from_user);
        return ERROR; 
    }
    pthread_mutex_lock(&(g_user_friendlock[i]));
    for(j=0;j<PNR_IMUSER_FRIENDS_MAXNUM;j++)
    {
        if(strcmp(g_imusr_array.usrnode[i].friends[j].user_toxid,to_user) == OK)
        {
        	g_imusr_array.usrnode[i].friends[j].oneway = 0;
			strncpy(g_imusr_array.usrnode[i].friends[j].user_nickname,nickname,TOX_ID_STR_LEN);
			strncpy(g_imusr_array.usrnode[i].friends[j].user_pubkey,userkey,PNR_USER_PUBKEY_MAXLEN);
            pthread_mutex_unlock(&(g_user_friendlock[i]));
            return ERROR; 
        }
        else if(none_index == 0 && g_imusr_array.usrnode[i].friends[j].exsit_flag == FALSE)
        {
            none_index = j;
            break;
        }
    }

    g_imusr_array.usrnode[i].friendnum++;
    f_id = get_indexbytoxid(to_user);
    if(f_id != 0)
    {
        g_imusr_array.usrnode[i].friends[none_index].local = TRUE;
    }
    else
    {
        g_imusr_array.usrnode[i].friends[none_index].local = FALSE;
    }
    g_imusr_array.usrnode[i].friends[none_index].exsit_flag = TRUE;
	g_imusr_array.usrnode[i].friends[none_index].oneway = 0;
    pnr_uidhash_get(i,none_index+1,g_imusr_array.usrnode[i].friends[none_index].user_toxid,
        &g_imusr_array.usrnode[i].friends[none_index].hashid,g_imusr_array.usrnode[i].friends[none_index].u_hashstr);
    //这里肯定是在线的
    g_imusr_array.usrnode[i].friends[none_index].online_status = USER_ONLINE_STATUS_ONLINE;
    strncpy(g_imusr_array.usrnode[i].friends[none_index].user_toxid,to_user,TOX_ID_STR_LEN);
    strncpy(g_imusr_array.usrnode[i].friends[none_index].user_nickname,nickname,TOX_ID_STR_LEN);
    strncpy(g_imusr_array.usrnode[i].friends[none_index].user_pubkey,userkey,PNR_USER_PUBKEY_MAXLEN);
    pthread_mutex_unlock(&(g_user_friendlock[i]));
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"add user(%d:%s),friend(%d:%s) nickname(%s) userkey(%s) friendnum(%d) hashid(%d:%s)",
        i,from_user,none_index,g_imusr_array.usrnode[i].friends[none_index].user_toxid,
        g_imusr_array.usrnode[i].friends[none_index].user_nickname,
        g_imusr_array.usrnode[i].friends[none_index].user_pubkey,g_imusr_array.usrnode[i].friendnum,
        g_imusr_array.usrnode[i].friends[none_index].hashid,g_imusr_array.usrnode[i].friends[none_index].u_hashstr);
    return OK;
}

/*****************************************************************************
 函 数 名  : im_nodelist_delfriend
 功能描述  : 删除好友节点
 输入参数  : int index        
             char* from_user  
             char* to_user    
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月10日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_nodelist_delfriend(int index,char* from_user,char* to_user,int oneway)
{
    int i = 0,j = 0,exsit_flag = 0;
    //检查是否非法
    if(from_user == NULL || to_user == NULL)
    {
        return ERROR;
    }

    i = get_indexbytoxid(from_user);
    if(i != index)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%s) not found",from_user);
        return ERROR;
    }

    pthread_mutex_lock(&(g_user_friendlock[i]));
    for(j=0;j<PNR_IMUSER_FRIENDS_MAXNUM;j++)
    {
        if(strcmp(g_imusr_array.usrnode[i].friends[j].user_toxid,to_user) == OK)
        {
            exsit_flag = TRUE;
            break;
        }
    }
    if(exsit_flag == TRUE)
    {
    	if (oneway) {
			g_imusr_array.usrnode[i].friends[j].oneway = 1;
		} else {
			g_imusr_array.usrnode[i].friendnum--;
			memset(&g_imusr_array.usrnode[i].friends[j],0,sizeof(struct im_friends_struct));
		}
    }
    else
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%d:%s) friend(%s) not found",i,from_user,to_user);
    }
    pthread_mutex_unlock(&(g_user_friendlock[i]));
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"del user(%d:%s),friend(%s)",index,from_user,to_user);
    return OK;
}

/**********************************************************************************
  Function:      im_pushmsg_callback
  Description:  im push 消息回调处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                     1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_pushmsg_callback(int index,int cmd,int local_flag,int apiversion,void* params)
{
    struct im_friend_msgstruct* pfriend = NULL;
    struct im_sendmsg_msgstruct* psendmsg = NULL;
	struct im_sendfile_struct *ptoxsendfile = NULL;
	struct im_user_msg_sendfile *psendfile = NULL;
	struct group_invite_info *pinviteinfo = NULL;
	struct group_user_msg* pgroupmsg = NULL;
	struct group_sys_msg* pgsysmsg = NULL;
	struct group_user* pgsender = NULL;
	struct group_user* pgrecer = NULL;
    struct group_fileinfo_struct tmp_finfo;
    char* pmsg = NULL;
    int msg_len = 0;
	char filepath[512] = {0};
	char fullfilename[512] = {0};
	char cmdbuf[1024] = {0};
	char md5[33] = {0};
	int findex = 0;
    int filesize = 0;
	int msgid = 0;
    char dpath[512] = {0};
    int pushmsg_ctype = 0;
	char fileinfo[PNR_FILEINFO_ATTACH_FLAG+1] = {0};
    char* ptmp = NULL;
    int fileinfo_len =0,attend_flag = 0;

	if(params == NULL)
    {
        return ERROR;
    }
	
    //构建消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if (ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)apiversion));
    
    switch(cmd)
    {
        case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
            pfriend = (struct im_friend_msgstruct*)params;
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_ADDFRIENDPUSH));
            cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(pfriend->touser_toxid));
            cJSON_AddItemToObject(ret_params, "FriendId",cJSON_CreateString(pfriend->fromuser_toxid));  
            cJSON_AddItemToObject(ret_params, "NickName",cJSON_CreateString(pfriend->nickname));
            cJSON_AddItemToObject(ret_params, "UserKey",cJSON_CreateString(pfriend->user_pubkey));
            cJSON_AddItemToObject(ret_params, "Msg",cJSON_CreateString(pfriend->friend_msg));
            cJSON_AddItemToObject(ret_params, "RouterId",cJSON_CreateString(g_daemon_tox.user_toxid));
            cJSON_AddItemToObject(ret_params, "RouterName",cJSON_CreateString(g_dev_nickname));
            break;
            
        case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
            pfriend = (struct im_friend_msgstruct*)params;
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_ADDFRIENDREPLY));
            cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(pfriend->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "FriendId",cJSON_CreateString(pfriend->touser_toxid));  
            cJSON_AddItemToObject(ret_params, "Nickname",cJSON_CreateString(pfriend->nickname));
            cJSON_AddItemToObject(ret_params, "Result",cJSON_CreateNumber(pfriend->result));
            cJSON_AddItemToObject(ret_params, "FriendName",cJSON_CreateString(pfriend->friend_nickname));
            cJSON_AddItemToObject(ret_params, "UserKey",cJSON_CreateString(pfriend->user_pubkey));
            if(apiversion == PNR_API_VERSION_V4)
            {
                cJSON_AddItemToObject(ret_params, "Sign",cJSON_CreateString(pfriend->sign));
            }
            cJSON_AddItemToObject(ret_params, "RouterId",cJSON_CreateString(g_daemon_tox.user_toxid));
            cJSON_AddItemToObject(ret_params, "RouterName",cJSON_CreateString(g_dev_nickname));
            //这里需要反向一下
            if(pfriend->result == OK && local_flag == TRUE)
            {
                pnr_friend_dbinsert(pfriend->touser_toxid,pfriend->fromuser_toxid,pfriend->nickname,pfriend->user_pubkey);
                im_nodelist_addfriend(index,pfriend->touser_toxid,pfriend->fromuser_toxid,pfriend->nickname,pfriend->user_pubkey);
                pnr_check_update_devinfo_bytoxid(index,pfriend->fromuser_toxid,g_daemon_tox.user_toxid,g_dev_nickname);
            }
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"index(%d %s->%s)",index,pfriend->touser_toxid,pfriend->fromuser_toxid);
            break;
           
        case PNR_IM_CMDTYPE_DELFRIENDPUSH:
            pfriend = (struct im_friend_msgstruct*)params;
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_DELFRIENDPUSH));
            cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(pfriend->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "FriendId",cJSON_CreateString(pfriend->touser_toxid));  

			if (local_flag == TRUE) {
				pnr_msgcache_dbdelete_by_friendid(index, pfriend->fromuser_toxid);
				im_nodelist_delfriend(index,pfriend->touser_toxid,pfriend->fromuser_toxid,1);
	            pnr_friend_dbdelete(pfriend->touser_toxid,pfriend->fromuser_toxid,1);
			}
			break;
            
        case PNR_IM_CMDTYPE_PUSHMSG:
            psendmsg = (struct im_sendmsg_msgstruct*)params;
            //目标对象是本地的时候，修改为先记录log，然后推送数据库id，
            if(local_flag == TRUE)
            {
                pnr_msglog_getid(index, &msgid);
                if(apiversion == PNR_API_VERSION_V1)
                {
                    pnr_msglog_dbinsert_specifyid(index,PNR_IM_MSGTYPE_TEXT,msgid,psendmsg->log_id,MSG_STATUS_SENDOK,psendmsg->fromuser_toxid,
                        psendmsg->touser_toxid,psendmsg->msg_buff,psendmsg->msg_srckey,psendmsg->msg_dstkey,NULL,0);                
                }
                else if(apiversion == PNR_API_VERSION_V3)
                {
                    pnr_msglog_dbinsert_specifyid_v3(index,PNR_IM_MSGTYPE_TEXT,msgid,psendmsg->log_id,MSG_STATUS_SENDOK,psendmsg->fromuser_toxid,
                        psendmsg->touser_toxid,psendmsg->msg_buff,psendmsg->sign,psendmsg->nonce,psendmsg->prikey,NULL,0);
                }
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"pushmsg: renew msgid(%d)",psendmsg->log_id);
            }
            else
            {
                msgid = psendmsg->log_id;
            }
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_PUSHMSG)); 
            cJSON_AddItemToObject(ret_params, "MsgId",cJSON_CreateNumber(msgid));  
            cJSON_AddItemToObject(ret_params, "Msg",cJSON_CreateString(psendmsg->msg_buff));  
            if(apiversion == PNR_API_VERSION_V1)
            {
                cJSON_AddItemToObject(ret_params, "FromId",cJSON_CreateString(psendmsg->fromuser_toxid));
                cJSON_AddItemToObject(ret_params, "ToId",cJSON_CreateString(psendmsg->touser_toxid));
                cJSON_AddItemToObject(ret_params, "SrcKey",cJSON_CreateString(psendmsg->msg_srckey));  
                cJSON_AddItemToObject(ret_params, "DstKey",cJSON_CreateString(psendmsg->msg_dstkey));  
            }
            else if(apiversion == PNR_API_VERSION_V3)
            {
                cJSON_AddItemToObject(ret_params, "Sign",cJSON_CreateString(psendmsg->sign));  
                cJSON_AddItemToObject(ret_params, "Nonce",cJSON_CreateString(psendmsg->nonce)); 
                cJSON_AddItemToObject(ret_params, "PriKey",cJSON_CreateString(psendmsg->prikey)); 
                char friend_pubkey[PNR_LOGINKEY_MAXLEN+1] = {0};
                if(pnr_friend_get_pubkey_bytoxid(psendmsg->touser_toxid,psendmsg->fromuser_toxid,friend_pubkey) == OK)
                {
                    cJSON_AddItemToObject(ret_params, "PubKey",cJSON_CreateString(friend_pubkey)); 
                }
#if 0 //暂时不用hashid
                if(local_flag == TRUE)
                {
                    int f_id = 0;
                    f_id = get_friendid_bytoxid(index,psendmsg->fromuser_toxid);
                    if(f_id >= 0 && f_id < PNR_IMUSER_FRIENDS_MAXNUM)
                    {
                        cJSON_AddItemToObject(ret_params, "From",cJSON_CreateString(g_imusr_array.usrnode[index].friends[f_id].u_hashstr));
                        cJSON_AddItemToObject(ret_params, "To",cJSON_CreateString(g_imusr_array.usrnode[index].u_hashstr));
                        DEBUG_PRINT(DEBUG_LEVEL_INFO,"PushMsg:renew hasdid(%s:%s->%s:%s)",
                            psendmsg->from_uid,psendmsg->to_uid,
                            g_imusr_array.usrnode[index].friends[f_id].u_hashstr,g_imusr_array.usrnode[index].u_hashstr);
                    }
                }
                else
                {
                    cJSON_AddItemToObject(ret_params, "From",cJSON_CreateString(psendmsg->from_uid));
                    cJSON_AddItemToObject(ret_params, "To",cJSON_CreateString(psendmsg->to_uid));
                }
#else
            cJSON_AddItemToObject(ret_params, "From",cJSON_CreateString(psendmsg->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "To",cJSON_CreateString(psendmsg->touser_toxid));
#endif
            }
            break;
        case PNR_IM_CMDTYPE_DELMSGPUSH:
            psendmsg = (struct im_sendmsg_msgstruct*)params;
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_DELMSGPUSH));
            cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(psendmsg->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "FriendId",cJSON_CreateString(psendmsg->touser_toxid));  
            //这里需要把最终推送给用户的msgid转换成数据库里面的id
            if(local_flag == TRUE)
            {
                pnr_msglog_dbget_dbid_bylogid(index,psendmsg->log_id,psendmsg->fromuser_toxid,psendmsg->touser_toxid,&msgid);
                pnr_msglog_dbdelete(index, 0, psendmsg->log_id, psendmsg->fromuser_toxid, psendmsg->touser_toxid);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"Delmsgpush:delete logid(%d) msgid(%d)",psendmsg->log_id,msgid);
            }
            else
            {
                msgid = psendmsg->log_id;
            }
            cJSON_AddItemToObject(ret_params, "MsgId",cJSON_CreateNumber(msgid));  
            break;   
            
        case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
            pfriend = (struct im_friend_msgstruct*)params;
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_ONLINESTATUSPUSH));
            cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(pfriend->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "RouterId",cJSON_CreateString(g_daemon_tox.user_toxid));
            cJSON_AddItemToObject(ret_params, "RouterName",cJSON_CreateString(g_dev_nickname));
            cJSON_AddItemToObject(ret_params, "OnlineStatus",cJSON_CreateNumber(pfriend->result));  
            break;

		case PNR_IM_CMDTYPE_PUSHFILE:
			psendfile = (struct im_user_msg_sendfile *)params;
            cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PUSHFILE));
            cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(psendfile->fromid));
            cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(psendfile->toid));
            ptmp =strchr(psendfile->filename,PNR_FILEINFO_ATTACH_FLAG);
            if(ptmp)
            {
                fileinfo_len = psendfile->filename+strlen(psendfile->filename)-ptmp;
                fileinfo_len = ((fileinfo_len >= PNR_FILEINFO_MAXLEN)?PNR_FILEINFO_MAXLEN:fileinfo_len);
                strncpy(fileinfo,ptmp,fileinfo_len);
			    cJSON_AddItemToObject(ret_params, "FileInfo", cJSON_CreateString(fileinfo+1));
                ptmp[0] = 0;
            }
		    cJSON_AddItemToObject(ret_params, "FileName", cJSON_CreateString(psendfile->filename));
            cJSON_AddItemToObject(ret_params, "FileType", cJSON_CreateNumber(ntohl(psendfile->action)));
            
			findex = get_indexbytoxid(psendfile->fromid);
            if(apiversion >= PNR_API_VERSION_V5)
            {
                //src file
                PNR_REAL_FILEPATH_GET(filepath,findex,PNR_FILE_SRCFROM_MSGSEND,htonl(psendfile->fileid),0,psendfile->filename);
                if(local_flag == TRUE)
                {
                    //目的文件
                    PNR_REAL_FILEPATH_GET(dpath,index,PNR_FILE_SRCFROM_MSGRECV,htonl(psendfile->fileid),0,psendfile->filename);
                    snprintf(cmdbuf, sizeof(cmdbuf), "cp %s%s %s%s", WS_SERVER_INDEX_FILEPATH,filepath,WS_SERVER_INDEX_FILEPATH,dpath);
                    system(cmdbuf);
                    strcpy(fullfilename,WS_SERVER_INDEX_FILEPATH);
                    strcat(fullfilename,dpath);
                    cJSON_AddItemToObject(ret_params, "FilePath", cJSON_CreateString(dpath));
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pushmsg_PushFile:(%s->%s)",filepath,dpath);
                }
                else
                {
                    strcpy(fullfilename,WS_SERVER_INDEX_FILEPATH);
                    strcat(fullfilename,filepath);
                    cJSON_AddItemToObject(ret_params, "FilePath", cJSON_CreateString(filepath));
                }
            }
            else
            {
    			snprintf(fullfilename, sizeof(fullfilename), "%ss/%s", 
				    g_imusr_array.usrnode[findex].userdata_pathurl, psendfile->filename);
                strcpy(filepath,fullfilename+strlen(WS_SERVER_INDEX_FILEPATH));
            }
            md5_hash_file(fullfilename, md5);
            filesize = im_get_file_size(fullfilename);
			DEBUG_PRINT(DEBUG_LEVEL_INFO, "file[%s]-filemd5[%s]", fullfilename, md5);
            cJSON_AddItemToObject(ret_params, "FileSize", cJSON_CreateNumber(filesize));
            cJSON_AddItemToObject(ret_params, "FileMD5", cJSON_CreateString(md5));
            cJSON_AddItemToObject(ret_params, "SrcKey", cJSON_CreateString(psendfile->srckey));
            cJSON_AddItemToObject(ret_params, "DstKey", cJSON_CreateString(psendfile->dstkey));
            //目标对象是本地的时候，修改为先记录log，然后推送数据库id，
            if(local_flag == TRUE)
            {
                pnr_msglog_getid(index, &msgid);
                if(fileinfo_len > 0)
                {
                    strcat(filepath,fileinfo);
                }
                pnr_msglog_dbinsert_specifyid(index,ntohl(psendfile->action),msgid,psendfile->magic,MSG_STATUS_SENDOK,psendfile->fromid,
                    psendfile->toid,psendfile->filename,psendfile->srckey,psendfile->dstkey,filepath,filesize);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"pushmsg: renew msgid(%d)",msgid);
            }
            else
            {
                msgid = psendfile->magic;
            }
            cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msgid));
			break;
        case PNR_IM_CMDTYPE_FILEFORWARD_PUSH:
            psendmsg = (struct im_sendmsg_msgstruct *)params;
            cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PUSHFILE));
            cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(psendmsg->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(psendmsg->touser_toxid));
            cJSON_AddItemToObject(ret_params, "FileName", cJSON_CreateString(psendmsg->msg_buff));
            cJSON_AddItemToObject(ret_params, "FileType", cJSON_CreateNumber(psendmsg->msgtype));
            snprintf(fullfilename, sizeof(fullfilename), "%s%s", DAEMON_PNR_USERDATA_DIR, psendmsg->ext);
            md5_hash_file(fullfilename, md5);
            DEBUG_PRINT(DEBUG_LEVEL_INFO, "file[%s]-filemd5[%s]", fullfilename, md5);
            cJSON_AddItemToObject(ret_params, "FileSize", cJSON_CreateNumber(psendmsg->ext2));
            cJSON_AddItemToObject(ret_params, "FileMD5", cJSON_CreateString(md5));
			cJSON_AddItemToObject(ret_params, "FilePath", cJSON_CreateString(psendmsg->ext));
            cJSON_AddItemToObject(ret_params, "SrcKey", cJSON_CreateString(psendmsg->sign));
            cJSON_AddItemToObject(ret_params, "DstKey", cJSON_CreateString(psendmsg->prikey));
            //目标对象是本地的时候，修改为先记录log，然后推送数据库id，
            if(local_flag == TRUE)
            {
                //转发的时候，logid不能要
                pnr_msglog_getid(index, &msgid);
                pnr_msglog_dbinsert_specifyid(index,psendmsg->msgtype,msgid,psendmsg->log_id,MSG_STATUS_SENDOK,psendmsg->fromuser_toxid,
                    psendmsg->touser_toxid,psendmsg->msg_buff,psendmsg->sign,psendmsg->prikey,psendmsg->ext,psendmsg->ext2);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"pushmsg: renew msgid(%d)",msgid);
            }
            else
            {
                msgid = psendmsg->log_id;
            }
            cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msgid));
            break;            
		case PNR_IM_CMDTYPE_PUSHFILE_TOX:
			ptoxsendfile = (struct im_sendfile_struct *)params;
			cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PUSHFILE));
            cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(ptoxsendfile->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(ptoxsendfile->touser_toxid));
			cJSON_AddItemToObject(ret_params, "FileName", cJSON_CreateString(ptoxsendfile->filename));
			cJSON_AddItemToObject(ret_params, "FileType", cJSON_CreateNumber(ptoxsendfile->filetype));
			findex = get_indexbytoxid(ptoxsendfile->fromuser_toxid);
			snprintf(fullfilename, sizeof(fullfilename), "%ss/%s", 
				g_imusr_array.usrnode[findex].userdata_pathurl, ptoxsendfile->filename);
			md5_hash_file(fullfilename, md5);
            filesize = im_get_file_size(fullfilename);
			DEBUG_PRINT(DEBUG_LEVEL_INFO, "file[%s]-filemd5[%s]", fullfilename, md5);
            cJSON_AddItemToObject(ret_params, "FileSize", cJSON_CreateNumber(filesize));
            cJSON_AddItemToObject(ret_params, "FileMD5", cJSON_CreateString(md5));
            cJSON_AddItemToObject(ret_params, "SrcKey", cJSON_CreateString(ptoxsendfile->srckey));
            cJSON_AddItemToObject(ret_params, "DstKey", cJSON_CreateString(ptoxsendfile->dstkey));
             //目标对象是本地的时候，修改为先记录log，然后推送数据库id，
            if(local_flag == TRUE)
            {
                pnr_msglog_getid(index, &msgid);
                pnr_msglog_dbinsert_specifyid(index,ptoxsendfile->filetype,msgid,ptoxsendfile->log_id,MSG_STATUS_SENDOK,ptoxsendfile->fromuser_toxid,
                    ptoxsendfile->touser_toxid,ptoxsendfile->filename,ptoxsendfile->srckey,ptoxsendfile->dstkey,fullfilename,filesize);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"pushmsg: renew msgid(%d)",msgid);
            }
            else
            {
                msgid = ptoxsendfile->log_id;
            }
            cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msgid));
			break;
			
        case PNR_IM_CMDTYPE_READMSGPUSH:
            psendmsg = (struct im_sendmsg_msgstruct*)params;
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_READMSGPUSH));
            cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(psendmsg->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "FriendId",cJSON_CreateString(psendmsg->touser_toxid));  
            cJSON_AddItemToObject(ret_params, "ReadMsgs",cJSON_CreateString(psendmsg->msg_buff));  
            if(local_flag == TRUE)
            {
               int tmp_msgid = 0;
               char* msgid_buff_end = NULL;
               char* tmp_msgid_head = NULL;
               msgid_buff_end = psendmsg->msg_buff + strlen(psendmsg->msg_buff);
               tmp_msgid_head = psendmsg->msg_buff;
               while(tmp_msgid_head != NULL)
               {
                   tmp_msgid = atoi(tmp_msgid_head);
                   if(tmp_msgid)
                   {
                       pnr_msglog_dbupdate_stauts_byid(index,tmp_msgid,MSG_STATUS_READ_OK);
                   }
                   tmp_msgid_head = strchr(tmp_msgid_head,',');
                   if(tmp_msgid_head)
                   {
                       tmp_msgid_head = tmp_msgid_head+1;
                       if(tmp_msgid_head >= msgid_buff_end)
                       {
                           break;
                       }
                   }
                   else
                   {
                       break;
                   }
               }
            }
            break;
        case PNR_IM_CMDTYPE_USERINFOPUSH:
            pfriend = (struct im_friend_msgstruct*)params;
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_USERINFOPUSH));
            cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(pfriend->touser_toxid));
            cJSON_AddItemToObject(ret_params, "FriendId",cJSON_CreateString(pfriend->fromuser_toxid));  
            cJSON_AddItemToObject(ret_params, "NickName",cJSON_CreateString(pfriend->friend_nickname));  
            break;
        case PNR_IM_CMDTYPE_VERIFYGROUPPUSH:
            pinviteinfo = (struct group_invite_info*)params;
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_VERIFYGROUPPUSH));
            cJSON_AddItemToObject(ret_params, "From",cJSON_CreateString(pinviteinfo->inviter));
            cJSON_AddItemToObject(ret_params, "To",cJSON_CreateString(pinviteinfo->user));  
            cJSON_AddItemToObject(ret_params, "Aduit",cJSON_CreateString(pinviteinfo->aduitor));
            cJSON_AddItemToObject(ret_params, "GId",cJSON_CreateString(pinviteinfo->groud_hid));
            cJSON_AddItemToObject(ret_params, "UserPubKey",cJSON_CreateString(pinviteinfo->user_pubkey));
            cJSON_AddItemToObject(ret_params, "UserGroupKey",cJSON_CreateString(pinviteinfo->user_groupkey));
            cJSON_AddItemToObject(ret_params, "FromName",cJSON_CreateString(pinviteinfo->inviter_name));
            cJSON_AddItemToObject(ret_params, "ToName",cJSON_CreateString(pinviteinfo->user_name));
            cJSON_AddItemToObject(ret_params, "Gname",cJSON_CreateString(pinviteinfo->group_name));
            if(pinviteinfo->msg[0] != 0)
            {
                cJSON_AddItemToObject(ret_params, "Msg",cJSON_CreateString(pinviteinfo->msg));
            }
            break;
        case PNR_IM_CMDTYPE_GROUPMSGPUSH:
            pgroupmsg = (struct group_user_msg*)params;
            pgsender = get_guserbyuindex(pgroupmsg->gid,pgroupmsg->from_uid);
            if(pgsender == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get pgsender null");
                return ERROR;
            }
            pnr_msglog_getid(index, &msgid);
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_GROUPMSGPUSH));
            cJSON_AddItemToObject(ret_params, "From",cJSON_CreateString(pgroupmsg->from));
            cJSON_AddItemToObject(ret_params, "To",cJSON_CreateString(pgroupmsg->to));
            cJSON_AddItemToObject(ret_params, "MsgType",cJSON_CreateNumber(pgroupmsg->type));
            if(pgroupmsg->attend_all == TRUE)
            {
                attend_flag = PNR_GROUP_MSGPUSH_ATTEND_ALL;
            }
            else if(pnr_stristr(pgroupmsg->attend,pgroupmsg->to) != NULL)
            {
                attend_flag = PNR_GROUP_MSGPUSH_ATTEND_ONLY;
            }
            else
            {
                attend_flag = PNR_GROUP_MSGPUSH_ATTEND_NONE;
            }
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"gmsg attend(%s:%s),attend_flag(%d)",pgroupmsg->attend,pgroupmsg->to,attend_flag);
            cJSON_AddItemToObject(ret_params, "Point",cJSON_CreateNumber(attend_flag));
            cJSON_AddItemToObject(ret_params, "GId",cJSON_CreateString(g_grouplist[pgroupmsg->gid].group_hid));
            cJSON_AddItemToObject(ret_params, "GroupName",cJSON_CreateString(pgroupmsg->group_name));
            cJSON_AddItemToObject(ret_params, "GAdmin",cJSON_CreateString(g_grouplist[pgroupmsg->gid].owner));
            cJSON_AddItemToObject(ret_params, "MsgId",cJSON_CreateNumber(pgroupmsg->msgid));
            cJSON_AddItemToObject(ret_params, "TimeStamp",cJSON_CreateNumber(pgroupmsg->timestamp));
            cJSON_AddItemToObject(ret_params, "UserName",cJSON_CreateString(pgsender->username));
            cJSON_AddItemToObject(ret_params, "UserKey",cJSON_CreateString(pgsender->user_pubkey));
            cJSON_AddItemToObject(ret_params, "SelfKey",cJSON_CreateString(pgroupmsg->to_key));
            if(pgroupmsg->type == PNR_IM_MSGTYPE_IMAGE || pgroupmsg->type == PNR_IM_MSGTYPE_AUDIO
                || pgroupmsg->type == PNR_IM_MSGTYPE_MEDIA || pgroupmsg->type == PNR_IM_MSGTYPE_FILE)
            {
                //添加文件信息
                cJSON_AddItemToObject(ret_params, "Msg",cJSON_CreateString(""));
                cJSON_AddItemToObject(ret_params, "FileName",cJSON_CreateString(pgroupmsg->msgpay));
                cJSON_AddItemToObject(ret_params, "FilePath",cJSON_CreateString(pgroupmsg->ext1));
                if(strlen(pgroupmsg->file_key) > 0)
                {
                    cJSON_AddItemToObject(ret_params, "FileKey",cJSON_CreateString(pgroupmsg->file_key));
                }
                //ext2 存储文件相对相关属性
                memset(&tmp_finfo,0,sizeof(tmp_finfo));
                if(im_group_fileinfo_analyze(pgroupmsg->ext2,&tmp_finfo) == OK)
                {
                    cJSON_AddItemToObject(ret_params, "FileMD5",cJSON_CreateString(tmp_finfo.md5));
                    cJSON_AddItemToObject(ret_params, "FileSize",cJSON_CreateNumber(tmp_finfo.filesize));
                    if(strlen(tmp_finfo.attach_info) > 0) 
                    {
                        cJSON_AddItemToObject(ret_params, "FileInfo",cJSON_CreateString(tmp_finfo.attach_info));
                    }
                }
                else
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_fileinfo_analyze:fileinfo(%s) failed",pgroupmsg->ext2);
                }
            }
            else
            {
                cJSON_AddItemToObject(ret_params, "FileName",cJSON_CreateString(""));
                cJSON_AddItemToObject(ret_params, "Msg",cJSON_CreateString(pgroupmsg->msgpay));
            }
            break;
        case PNR_IM_CMDTYPE_GROUPSYSPUSH:
            pgsysmsg = (struct group_sys_msg*)params;
            pgsender = get_guserbyuindex(pgsysmsg->gid,pgsysmsg->from_uid);
            if(pgsender == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"gid(%d) form_id(%d) err",pgsysmsg->gid,pgsysmsg->from_uid);
                return ERROR;
            }
            cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_GROUPSYSPUSH));
            cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(pgsysmsg->msgtargetuser));
            cJSON_AddItemToObject(ret_params, "GId",cJSON_CreateString(pgsysmsg->group_hid));
            cJSON_AddItemToObject(ret_params, "Type",cJSON_CreateNumber(pgsysmsg->type));
            cJSON_AddItemToObject(ret_params, "MsgId",cJSON_CreateNumber(pgsysmsg->msgid));
            cJSON_AddItemToObject(ret_params, "From",cJSON_CreateString(pgsender->toxid));
            cJSON_AddItemToObject(ret_params, "FromUserName",cJSON_CreateString(pgsender->username));
            switch(pgsysmsg->type)
            {
                case GROUP_SYSMSG_REMARK_GROUPNAME:
                case GROUP_SYSMSG_DISGROUP:
                    cJSON_AddItemToObject(ret_params, "Name",cJSON_CreateString(pgsysmsg->msgpay));
                    break;
                case GROUP_SYSMSG_VERIFY_MODIFY:
                    cJSON_AddItemToObject(ret_params, "NeedVerify",cJSON_CreateNumber(pgsysmsg->result));
                    break;
                case GROUP_SYSMSG_DELMSG_BYSELF:
                    break;
                case GROUP_SYSMSG_DELMSG_BYADMIN:
                case GROUP_SYSMSG_NEWUSER:
                case GROUP_SYSMSG_KICKOFF:
                    pgrecer = get_guserbyuindex(pgsysmsg->gid,pgsysmsg->to_uid);
                    if(pgrecer == NULL)
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"gid(%d) to_id(%d) err",pgsysmsg->gid,pgsysmsg->from_uid);
                        return ERROR;
                    }
                    else
                    {
                        cJSON_AddItemToObject(ret_params, "To",cJSON_CreateString(pgrecer->toxid));
                        cJSON_AddItemToObject(ret_params, "ToUserName",cJSON_CreateString(pgrecer->username));
                        cJSON_AddItemToObject(ret_params, "UserKey",cJSON_CreateString(pgrecer->user_pubkey));
                    }
                    break;
                case GROUP_SYSMSG_SELFOUT:
                    break;
                default:
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad syscmd type(%d)",pgsysmsg->type);
                    return ERROR;
            }
            break;
        default:
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"bad cmd(%d)",cmd);
            cJSON_Delete(ret_root);
            return ERROR;
    }

    pnr_msgcache_getid(index, &msgid);
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    if (local_flag == TRUE) {
        if ((cmd == PNR_IM_CMDTYPE_PUSHFILE || cmd == PNR_IM_CMDTYPE_PUSHFILE_TOX) 
            && (apiversion < PNR_API_VERSION_V5))
            //新的版本本地的就不在这里拷贝了
        {
			char *fname = NULL;
			if (cmd == PNR_IM_CMDTYPE_PUSHFILE) {
				fname = psendfile->filename;
			} else {
				fname = ptoxsendfile->filename;
			}
			snprintf(filepath, sizeof(filepath), "%sr/%s", 
				g_imusr_array.usrnode[index].userdata_pathurl, fname);
            //去掉附加信息
            ptmp = strchr(fullfilename,PNR_FILEINFO_ATTACH_FLAG);
            if(ptmp != NULL)
            {
                ptmp[0] = 0;
            }
			snprintf(cmdbuf, sizeof(cmdbuf), "cp %s %s", fullfilename, filepath);
			system(cmdbuf);
			snprintf(dpath, sizeof(dpath), "/user%d/r/%s", index, fname);
			cJSON_AddItemToObject(ret_params, "FilePath", cJSON_CreateString(dpath));
			DEBUG_PRINT(DEBUG_LEVEL_INFO, "file[%s]-filepath[%s]", fullfilename, dpath);
        }
        
		cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber(msgid));
		//这里消息内容不能做转义，要不然对端收到会出错
	    pmsg = cJSON_PrintUnformatted_noescape(ret_root);
	    cJSON_Delete(ret_root);
	    msg_len = strlen(pmsg);
	    if (msg_len < TOX_ID_STR_LEN || msg_len >= IM_JSON_MAXLEN) {
	        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",msg_len);
	        free(pmsg);
            pnr_msgcache_dbdelete(msgid, 0);
	        return ERROR;
	    }

        if(g_imusr_array.usrnode[index].user_online_type == USER_ONLINE_TYPE_LWS)
        {
            pushmsg_ctype = PNR_MSG_CACHE_TYPE_LWS;
        }
        else
        {
            pushmsg_ctype = PNR_MSG_CACHE_TYPE_TOXA;
        }
		
        switch (cmd) {
        case PNR_IM_CMDTYPE_PUSHFILE:
			pnr_msgcache_dbinsert(msgid, psendfile->fromid, psendfile->toid, cmd,
                pmsg, msg_len, psendfile->filename, fullfilename, psendfile->magic, 
                pushmsg_ctype, ntohl(psendfile->action),psendfile->srckey,psendfile->dstkey);
            break;
		case PNR_IM_CMDTYPE_FILEFORWARD_PUSH:
			pnr_msgcache_dbinsert(msgid, psendmsg->fromuser_toxid, psendmsg->touser_toxid, PNR_IM_CMDTYPE_PUSHFILE,
                pmsg, msg_len, psendmsg->msg_buff, fullfilename, psendmsg->log_id, 
                pushmsg_ctype, ntohl(psendmsg->msgtype),psendmsg->sign,psendmsg->prikey);
            break;
		case PNR_IM_CMDTYPE_PUSHFILE_TOX:
            pnr_msgcache_dbinsert(msgid, ptoxsendfile->fromuser_toxid, ptoxsendfile->touser_toxid, cmd,
                pmsg, msg_len, ptoxsendfile->filename, fullfilename, ptoxsendfile->log_id, 
                pushmsg_ctype, ptoxsendfile->filetype, ptoxsendfile->srckey, ptoxsendfile->dstkey);
            break;
        case PNR_IM_CMDTYPE_PUSHMSG:
            if(apiversion == PNR_API_VERSION_V1)
            {
                pnr_msgcache_dbinsert(msgid, psendmsg->fromuser_toxid, 
                    psendmsg->touser_toxid, cmd, pmsg, msg_len, NULL, NULL, psendmsg->log_id, 
                    pushmsg_ctype, PNR_IM_MSGTYPE_TEXT,psendmsg->msg_srckey,psendmsg->msg_dstkey);
            }
            else if(apiversion == PNR_API_VERSION_V3)
            {
                pnr_msgcache_dbinsert_v3(msgid, psendmsg->fromuser_toxid, 
                    psendmsg->touser_toxid, cmd, pmsg, msg_len, NULL, NULL, psendmsg->log_id, 
                    pushmsg_ctype, PNR_IM_MSGTYPE_TEXT,psendmsg->sign,psendmsg->nonce,psendmsg->prikey);
            }
            break;
        case PNR_IM_CMDTYPE_DELMSGPUSH:
        case PNR_IM_CMDTYPE_READMSGPUSH:
            pnr_msgcache_dbinsert(msgid, psendmsg->fromuser_toxid, 
                psendmsg->touser_toxid, cmd, pmsg, msg_len, NULL, NULL, psendmsg->log_id, 
                pushmsg_ctype, PNR_IM_MSGTYPE_TEXT,psendmsg->msg_srckey,psendmsg->msg_dstkey);
            break;

		case PNR_IM_CMDTYPE_DELFRIENDPUSH:
			break;

        case PNR_IM_CMDTYPE_GROUPMSGPUSH:
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"%s->%s,pushmsg_ctype(%d)type(%d)",pgroupmsg->from,pgroupmsg->to,pushmsg_ctype, pgroupmsg->type);
            pnr_msgcache_dbinsert(msgid,pgroupmsg->from,pgroupmsg->to,cmd,pmsg, msg_len,
                NULL, NULL, 0, pushmsg_ctype, pgroupmsg->type,NULL,NULL);
            break;
        case PNR_IM_CMDTYPE_VERIFYGROUPPUSH:
            pnr_msgcache_dbinsert(msgid,pinviteinfo->inviter,pinviteinfo->aduitor,cmd,pmsg, msg_len,
                NULL, NULL, 0, pushmsg_ctype, 0,NULL,NULL);
            break;
        case PNR_IM_CMDTYPE_GROUPSYSPUSH:
            pnr_msgcache_dbinsert(msgid,pgsysmsg->from_user,pgsysmsg->msgtargetuser,cmd,pmsg, msg_len,
                NULL, NULL, 0, pushmsg_ctype, 0,NULL,NULL);
            break;
        default:
            pnr_msgcache_dbinsert(msgid, pfriend->fromuser_toxid, 
                pfriend->touser_toxid, cmd, pmsg, msg_len, NULL, NULL, 0, 
                pushmsg_ctype, PNR_IM_MSGTYPE_TEXT,NULL,NULL);
        }
     } 
     else 
     {
		//这里消息内容不能做转义，要不然对端收到会出错
        pmsg = cJSON_PrintUnformatted_noescape(ret_root);
	    cJSON_Delete(ret_root);
	    msg_len = strlen(pmsg);
	    if (msg_len < TOX_ID_STR_LEN || msg_len >= IM_JSON_MAXLEN) {
	        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",msg_len);
	        free(pmsg);
            pnr_msgcache_dbdelete(msgid, 0);
	        return ERROR;
	    }
	
    	switch (cmd) {
		case PNR_IM_CMDTYPE_PUSHMSG:
#if (DB_CURRENT_VERSION < DB_VERSION_V3)
#else
			insert_tox_msgnode_v3(index, psendmsg->fromuser_toxid,
                psendmsg->touser_toxid, pmsg, msg_len, cmd, psendmsg->log_id,
                msgid, psendmsg->sign, psendmsg->nonce, psendmsg->prikey);
            break;
#endif
		case PNR_IM_CMDTYPE_DELMSGPUSH:
        case PNR_IM_CMDTYPE_READMSGPUSH:
			insert_tox_msgnode(index, psendmsg->fromuser_toxid,
                psendmsg->touser_toxid, pmsg, msg_len, cmd, psendmsg->log_id,
                msgid, psendmsg->msg_srckey, psendmsg->msg_dstkey);
			break;
			
		case PNR_IM_CMDTYPE_PUSHFILE:
			insert_tox_file_msgnode(index, psendfile->fromid, psendfile->toid, 
                pmsg, msg_len, psendfile->filename, fullfilename, cmd, psendfile->fileid, 
                msgid, ntohl(psendfile->action),psendfile->srckey, psendfile->dstkey);
			break;
			
		case PNR_IM_CMDTYPE_PUSHFILE_TOX:
			insert_tox_file_msgnode(index, ptoxsendfile->fromuser_toxid, ptoxsendfile->touser_toxid, 
                pmsg, msg_len, ptoxsendfile->filename, fullfilename, cmd, ptoxsendfile->log_id, 
                msgid, ptoxsendfile->filetype, ptoxsendfile->srckey, ptoxsendfile->dstkey);
			break;

		case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
			add_friends_force(g_tox_linknode[index], pfriend->touser_toxid, pmsg);
			break;

		case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
			add_friends_force(g_tox_linknode[index], pfriend->touser_toxid, pmsg);
			insert_tox_msgnode(index, pfriend->fromuser_toxid,
                pfriend->touser_toxid, pmsg, msg_len, cmd, 0, msgid,NULL,NULL);
			break;
			
		default:
			insert_tox_msgnode(index, pfriend->fromuser_toxid,
                pfriend->touser_toxid, pmsg, msg_len, cmd, 0, msgid,NULL,NULL);
		}
    }

    free(pmsg);
    return OK;
}

/*****************************************************************************
 函 数 名  : im_tox_pushmsg_callback
 功能描述  : tox转发消息
 输入参数  : int index     
             int cmd       
             void *params  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月25日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_tox_pushmsg_callback(int index, int cmd, int apiversion, void *params)
{
    struct im_friend_msgstruct *pfriend = (struct im_friend_msgstruct *)params;
    struct im_sendmsg_msgstruct *psendmsg = (struct im_sendmsg_msgstruct *)params;
	struct im_user_msg_sendfile *psendfile = (struct im_user_msg_sendfile *)params;
    struct im_sendfile_struct *pfile = (struct im_sendfile_struct *)params;
    char* pmsg = NULL;
    int msg_len = 0;
	char fullfilename[512] = {0};
	char md5[33] = {0};
	int findex = 0;
	int msgid = 0;
    char fileinfo[PNR_FILEINFO_MAXLEN+1] = {0};
    char* ptmp = NULL;
    int fileinfo_len =0;

	if (!params) {
        return ERROR;
    }
	
	cJSON *ret_root = cJSON_CreateObject();
    cJSON *ret_params = cJSON_CreateObject();
    if (!ret_root || !ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)apiversion));
    
    switch (cmd) {
    case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
        cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_ADDFRIENDPUSH));
        cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(pfriend->touser_toxid));
        cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(pfriend->fromuser_toxid));  
        cJSON_AddItemToObject(ret_params, "NickName", cJSON_CreateString(pfriend->nickname));
        cJSON_AddItemToObject(ret_params, "UserKey",cJSON_CreateString(pfriend->user_pubkey));
        cJSON_AddItemToObject(ret_params, "Msg",cJSON_CreateString(pfriend->friend_msg));
        cJSON_AddItemToObject(ret_params, "RouterId",cJSON_CreateString(g_daemon_tox.user_toxid));
        cJSON_AddItemToObject(ret_params, "RouterName",cJSON_CreateString(g_dev_nickname));
        break;
        
    case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
        cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_ADDFRIENDREPLY));
        cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(pfriend->fromuser_toxid));
        cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(pfriend->touser_toxid));  
        cJSON_AddItemToObject(ret_params, "Nickname", cJSON_CreateString(pfriend->nickname));
        cJSON_AddItemToObject(ret_params, "Result", cJSON_CreateNumber(pfriend->result));
        cJSON_AddItemToObject(ret_params, "FriendName", cJSON_CreateString(pfriend->friend_nickname));
        cJSON_AddItemToObject(ret_params, "UserKey", cJSON_CreateString(pfriend->user_pubkey));
        cJSON_AddItemToObject(ret_params, "RouterId",cJSON_CreateString(g_daemon_tox.user_toxid));
        cJSON_AddItemToObject(ret_params, "RouterName",cJSON_CreateString(g_dev_nickname));
        //这里需要反向一下
        if (pfriend->result == OK) {
            pnr_friend_dbinsert(pfriend->touser_toxid,pfriend->fromuser_toxid,pfriend->nickname,pfriend->user_pubkey);
            im_nodelist_addfriend(index,pfriend->touser_toxid,pfriend->fromuser_toxid,pfriend->nickname,pfriend->user_pubkey);
            pnr_check_update_devinfo_bytoxid(index,pfriend->fromuser_toxid,g_daemon_tox.user_toxid,g_dev_nickname);
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"index(%d %s->%s)",index,pfriend->touser_toxid,pfriend->fromuser_toxid);
        break;
       
    case PNR_IM_CMDTYPE_DELFRIENDPUSH:
        cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_DELFRIENDPUSH));
        cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(pfriend->fromuser_toxid));
        cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(pfriend->touser_toxid));  
		break;
        
    case PNR_IM_CMDTYPE_PUSHMSG:
        cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PUSHMSG));
        cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(psendmsg->log_id));  
        cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(psendmsg->msg_buff));  
        if(apiversion == PNR_API_VERSION_V1)
        {
            cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(psendmsg->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(psendmsg->touser_toxid));  
            cJSON_AddItemToObject(ret_params, "SrcKey", cJSON_CreateString(psendmsg->msg_srckey));  
            cJSON_AddItemToObject(ret_params, "DstKey", cJSON_CreateString(psendmsg->msg_dstkey));  
        }
        else if(apiversion == PNR_API_VERSION_V3)
        {
#if 0 //暂时不用hashid
            cJSON_AddItemToObject(ret_params, "From", cJSON_CreateString(psendmsg->from_uid));
            cJSON_AddItemToObject(ret_params, "To", cJSON_CreateString(psendmsg->to_uid));  
#else
            cJSON_AddItemToObject(ret_params, "From", cJSON_CreateString(psendmsg->fromuser_toxid));
            cJSON_AddItemToObject(ret_params, "To", cJSON_CreateString(psendmsg->touser_toxid));  
#endif
            cJSON_AddItemToObject(ret_params, "Sign", cJSON_CreateString(psendmsg->sign));  
            cJSON_AddItemToObject(ret_params, "Nonce", cJSON_CreateString(psendmsg->nonce));
            cJSON_AddItemToObject(ret_params, "PriKey", cJSON_CreateString(psendmsg->prikey));  
            char friend_pubkey[PNR_LOGINKEY_MAXLEN+1] = {0};
            if(pnr_friend_get_pubkey_bytoxid(psendmsg->touser_toxid,psendmsg->fromuser_toxid,friend_pubkey) == OK)
            {
                cJSON_AddItemToObject(ret_params, "PubKey",cJSON_CreateString(friend_pubkey)); 
            }
        }
        break;
     case PNR_IM_CMDTYPE_READMSGPUSH:
        psendmsg = (struct im_sendmsg_msgstruct*)params;
        cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_READMSGPUSH));
        cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(psendmsg->fromuser_toxid));
        cJSON_AddItemToObject(ret_params, "FriendId",cJSON_CreateString(psendmsg->touser_toxid));  
        cJSON_AddItemToObject(ret_params, "ReadMsgs",cJSON_CreateString(psendmsg->msg_buff));  
        break;   
      case PNR_IM_CMDTYPE_USERINFOPUSH:
        pfriend = (struct im_friend_msgstruct*)params;
        cJSON_AddItemToObject(ret_params, "Action",cJSON_CreateString(PNR_IMCMD_USERINFOPUSH));
        cJSON_AddItemToObject(ret_params, "UserId",cJSON_CreateString(pfriend->touser_toxid));
        cJSON_AddItemToObject(ret_params, "FriendId",cJSON_CreateString(pfriend->fromuser_toxid));  
        cJSON_AddItemToObject(ret_params, "NickName",cJSON_CreateString(pfriend->friend_nickname));  
        break;  
    case PNR_IM_CMDTYPE_DELMSGPUSH:
        cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_DELMSGPUSH));
        cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(psendmsg->fromuser_toxid));
        cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(psendmsg->touser_toxid));  
        cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(psendmsg->log_id));  
        break;   
        
    case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
        cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_ONLINESTATUSPUSH));
        cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(pfriend->fromuser_toxid));
        cJSON_AddItemToObject(ret_params, "OnlineStatus", cJSON_CreateNumber(pfriend->result));  
        break;

	case PNR_IM_CMDTYPE_PUSHFILE:
		cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PUSHFILE));
        cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(psendfile->fromid));
        cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(psendfile->toid));
        ptmp = strchr(pfile->filename,PNR_FILEINFO_ATTACH_FLAG);
        if(ptmp != NULL)
        {
            fileinfo_len = pfile->filename+strlen(pfile->filename)-ptmp;
            fileinfo_len = ((fileinfo_len >= PNR_FILEINFO_MAXLEN)?PNR_FILEINFO_MAXLEN:fileinfo_len);
            strncpy(fileinfo,ptmp,fileinfo_len);
            ptmp[0] = 0;
            cJSON_AddItemToObject(ret_params, "FileInfo", cJSON_CreateString(fileinfo+1));
        }
        cJSON_AddItemToObject(ret_params, "FileName", cJSON_CreateString(psendfile->filename));
		cJSON_AddItemToObject(ret_params, "FileType", cJSON_CreateNumber(ntohl(psendfile->action)));
		findex = get_indexbytoxid(psendfile->fromid);
		snprintf(fullfilename, sizeof(fullfilename), "%ss/%s", 
			g_imusr_array.usrnode[findex].userdata_pathurl, psendfile->filename);
		md5_hash_file(fullfilename, md5);
		DEBUG_PRINT(DEBUG_LEVEL_INFO, "file[%s]-filemd5[%s]", fullfilename, md5);

        cJSON_AddItemToObject(ret_params, "FileSize", cJSON_CreateNumber(im_get_file_size(fullfilename)));
        cJSON_AddItemToObject(ret_params, "FileMD5", cJSON_CreateString(md5));
		cJSON_AddItemToObject(ret_params, "SrcKey", cJSON_CreateString(psendfile->srckey));
		cJSON_AddItemToObject(ret_params, "DstKey", cJSON_CreateString(psendfile->dstkey));
        cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(psendfile->fileid));
		break;

    case PNR_IM_CMDTYPE_PUSHFILE_TOX:
        cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PUSHFILE));
        cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(pfile->fromuser_toxid));
        cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(pfile->touser_toxid));
		cJSON_AddItemToObject(ret_params, "FileName", cJSON_CreateString(pfile->filename));
        cJSON_AddItemToObject(ret_params, "FileType", cJSON_CreateNumber(pfile->filetype));
        cJSON_AddItemToObject(ret_params, "FileSize", cJSON_CreateNumber(pfile->filesize));
        cJSON_AddItemToObject(ret_params, "FileMD5", cJSON_CreateString(pfile->md5));
		cJSON_AddItemToObject(ret_params, "SrcKey", cJSON_CreateString(pfile->srckey));
		cJSON_AddItemToObject(ret_params, "DstKey", cJSON_CreateString(pfile->dstkey));
		cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(pfile->log_id));

		findex = get_indexbytoxid(pfile->fromuser_toxid);
		snprintf(fullfilename, sizeof(fullfilename), "%ss/%s", 
			g_imusr_array.usrnode[findex].userdata_pathurl, pfile->filename);
		md5_hash_file(fullfilename, md5);
		DEBUG_PRINT(DEBUG_LEVEL_INFO, "file[%s]-filemd5[%s]", fullfilename, md5);
        break;
        
    default:
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"bad cmd(%d)",cmd);
        cJSON_Delete(ret_root);
        return ERROR;
    }

    pnr_msgcache_getid(index, &msgid);
    cJSON_AddItemToObject(ret_root, "params", ret_params);
	
	//这里消息内容不能做转义，要不然对端收到会出错
    pmsg = cJSON_PrintUnformatted_noescape(ret_root);
    cJSON_Delete(ret_root);
    msg_len = strlen(pmsg);
    if (msg_len < TOX_ID_STR_LEN || msg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",msg_len);
        free(pmsg);
        pnr_msgcache_dbdelete(msgid, 0);
        return ERROR;
    }

	switch (cmd) {
	case PNR_IM_CMDTYPE_PUSHMSG:
        if(apiversion == PNR_API_VERSION_V1)
        {
    		insert_tox_msgnode(index, psendmsg->fromuser_toxid,
                psendmsg->touser_toxid, pmsg, msg_len, cmd, psendmsg->log_id,
                msgid, psendmsg->msg_srckey, psendmsg->msg_dstkey);
        }
        else if(apiversion == PNR_API_VERSION_V3)
        {
            insert_tox_msgnode_v3(index, psendmsg->fromuser_toxid,
                    psendmsg->touser_toxid, pmsg, msg_len, cmd, psendmsg->log_id,
                    msgid, psendmsg->sign, psendmsg->nonce,psendmsg->prikey);
        }
        break;
	case PNR_IM_CMDTYPE_DELMSGPUSH:
    case PNR_IM_CMDTYPE_READMSGPUSH:
		insert_tox_msgnode(index, psendmsg->fromuser_toxid,
            psendmsg->touser_toxid, pmsg, msg_len, cmd, psendmsg->log_id,
            msgid, psendmsg->msg_srckey, psendmsg->msg_dstkey);
		break;
		
	case PNR_IM_CMDTYPE_PUSHFILE:
		insert_tox_file_msgnode(index, psendfile->fromid, psendfile->toid, 
            pmsg, msg_len, psendfile->filename, fullfilename, cmd, psendfile->fileid, 
            msgid, ntohl(psendfile->action),psendfile->srckey, psendfile->dstkey);
		break;

	case PNR_IM_CMDTYPE_PUSHFILE_TOX:
		insert_tox_file_msgnode(index, pfile->fromuser_toxid, pfile->touser_toxid, 
            pmsg, msg_len, pfile->filename, fullfilename, cmd, pfile->log_id, 
            msgid, pfile->filetype, pfile->srckey, pfile->dstkey);
		break;

	case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
		add_friends_force(g_tox_linknode[index], pfriend->touser_toxid, pmsg);
		break;

	case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
		add_friends_force(g_tox_linknode[index], pfriend->touser_toxid, pmsg);
		insert_tox_msgnode(index, pfriend->fromuser_toxid,
            pfriend->touser_toxid, pmsg, msg_len, cmd, 0, msgid,NULL,NULL);
		break;

	default:
		insert_tox_msgnode(index, pfriend->fromuser_toxid,
            pfriend->touser_toxid, pmsg, msg_len, cmd, 0, msgid, "", "");
	}

    free(pmsg);
    return OK;
}
/**********************************************************************************
  Function:      imstance_daemon
  Description:  im tox 实例进程
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                     1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
void *imstance_daemon(void *para)
{
    int index = *(int*)para;
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"imstance_daemon user(%d) init",index);
    CreatedP2PNetwork_new(index);
	return NULL;
}

/**********************************************************************************
  Function:      im_userlogin_deal
  Description: IM模块LOGIN消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_userlogin_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char user_id[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int data_version = 0;
    int ret_code = 0;
    int need_synch = 0;
    char* ret_buff = NULL;
    int index = 0,run = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_id,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserDataVersion",data_version,TOX_ID_STR_LEN);

    if(g_p2pnet_init_flag != TRUE)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"g_p2pnet_init_flag no ok");
        ret_code = PNR_USER_LOGIN_NO_SERVER;
        need_synch = FALSE;
    }
    //检测是否是本地路由授权用户
    else if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"routid(%s) user input(%s)",
            g_daemon_tox.user_toxid,router_id);
        ret_code = PNR_USER_LOGIN_NO_SERVER;
        need_synch = FALSE;
    }
    //新用户的处理
    else if(strlen(user_id) == 0)
    {
        //如果当前还有可分配新用户
        if(g_imusr_array.cur_user_num < g_imusr_array.max_user_num)
        {
            for(index=1;index<=g_imusr_array.max_user_num;index++)
            {
                if(g_imusr_array.usrnode[index].user_toxid[0] == 0)
                {   
                    break;
                }
            }
            if(index <= g_imusr_array.max_user_num)
            {
                //启动im server进程
                //g_tmp_instance_index = index;
                if (pthread_create(&g_imusr_array.usrnode[index].tox_tid, NULL, imstance_daemon, &index) != 0) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                    ret_code = PNR_USER_LOGIN_OTHER_ERR;
                    need_synch = FALSE;
                }  
                else
                {
                    while(g_imusr_array.usrnode[index].init_flag != TRUE && run < 5)
                    {
                        sleep(1);
                        run++;
                    }
                    if(run >= 5)
                    {
                        ret_code = PNR_USER_LOGIN_OTHER_ERR;
                        need_synch = FALSE;
                    }
                    else
                    {
                        *plws_index = index;
                        ret_code = PNR_USER_LOGIN_OK;
                        need_synch = FALSE;
                        strcpy(user_id,g_imusr_array.usrnode[index].user_toxid);
                    }
                }
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get idel index failed");
                ret_code = PNR_USER_LOGIN_NO_FREE_USER;
                need_synch = FALSE;
            }
        }
        else
        {
            ret_code = PNR_USER_LOGIN_NO_FREE_USER;
            need_synch = FALSE;
        }
    }
    //老用户的处理
    else if(strlen(user_id) == TOX_ID_STR_LEN)
    {
        //查询是否已经存在的实例
        for(index=1;index<=g_imusr_array.max_user_num;index++)
        {
            if(strcmp(user_id,g_imusr_array.usrnode[index].user_toxid) == OK)
            {
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"get user_id(%s) index(%d)",user_id,index);
                break;
            }
        }
        //可能是其他路由器上实例过,所以再重新生成一个
        if(index > g_imusr_array.max_user_num)
        {
            for(index=1;index<=g_imusr_array.max_user_num;index++)
            {
                if(g_imusr_array.usrnode[index].user_toxid[0] == 0)
                {   
                    break;
                }
            }
            if(index <= g_imusr_array.max_user_num)
            {
                strcpy(g_imusr_array.usrnode[index].user_toxid,user_id);
                //启动im server进程
                //g_tmp_instance_index = index;
                if (pthread_create(&g_imusr_array.usrnode[index].tox_tid, NULL, imstance_daemon, &index) != 0) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                    ret_code = PNR_USER_LOGIN_OTHER_ERR;
                    need_synch = FALSE;
                }  
                else
                {
                    while(g_imusr_array.usrnode[index].init_flag != TRUE && run < 5)
                    {
                        sleep(1);
                        run++;
                    }
                    if(run >= 5)
                    {
                        ret_code = PNR_USER_LOGIN_OTHER_ERR;
                        need_synch = FALSE;
                    }
                    else
                    {
                        *plws_index = index;
                        ret_code = PNR_USER_LOGIN_OK;
                        need_synch = FALSE;
                        strcpy(user_id,g_imusr_array.usrnode[index].user_toxid);
                        DEBUG_PRINT(DEBUG_LEVEL_INFO,"renew user(%s)",user_id);
                    }
                }
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get idel index failed");
                ret_code = PNR_USER_LOGIN_NO_FREE_USER;
                need_synch = FALSE;
            }
        }
        else
        {
            if(g_imusr_array.usrnode[index].init_flag == FALSE)
            {
                //DEBUG_PRINT(DEBUG_LEVEL_INFO,"index(%d) init_flag(%d)",index,g_imusr_array.usrnode[index].init_flag);                
                //g_tmp_instance_index = index;
                if (pthread_create(&g_imusr_array.usrnode[index].tox_tid, NULL, imstance_daemon, &index) != 0) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                    ret_code = PNR_USER_LOGIN_OTHER_ERR;
                    need_synch = FALSE;
                } 
                else
                {
                    while(g_imusr_array.usrnode[index].init_flag != TRUE && run < 5)
                    {
                        sleep(1);
                        run++;
                    }
                    if(run >= 5)
                    {
                        ret_code = PNR_USER_LOGIN_OTHER_ERR;
                        need_synch = FALSE;
                    }
                    else
                    {
                        *plws_index = index;
                        ret_code = PNR_USER_LOGIN_OK;
                        need_synch = FALSE;
                        strcpy(user_id,g_imusr_array.usrnode[index].user_toxid);
                        DEBUG_PRINT(DEBUG_LEVEL_INFO,"renew user(%s)",user_id);
                    }
                }
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"create user(%s) relogin",user_id);
                ret_code = PNR_USER_LOGIN_OK;
                *plws_index = index;
                need_synch = FALSE;
            }
        }
    }
    else
    {
        ret_code = PNR_USER_LOGIN_OTHER_ERR;
        need_synch = FALSE;
    }

    //成功登陆
    if(ret_code == PNR_USER_LOGIN_OK)
    {
        imuser_friendstatus_push(index,USER_ONLINE_STATUS_ONLINE);
        g_imusr_array.usrnode[index].heartbeat_count= 0;
    }
    
    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_LOGIN));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(user_id));
    cJSON_AddItemToObject(ret_params, "NeedSynch", cJSON_CreateNumber(need_synch));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_userdestory_deal
  Description: IM模块Destory消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_userdestory_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char user_id[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_id,TOX_ID_STR_LEN);

    if(g_p2pnet_init_flag != TRUE)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"g_p2pnet_init_flag no ok");
        ret_code = PNR_USER_DESTORY_OTHER_ERR;
    }
    //检测是否是本地路由授权用户
    else if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"routid(%s) user input(%s)",
            g_daemon_tox.user_toxid,router_id);
        ret_code = PNR_USER_DESTORY_BAD_ROUTERID;
    }
    //useid 处理
    else if(strlen(user_id) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s)",router_id);
        ret_code = PNR_USER_DESTORY_BAD_USERID;
    }
    else
    {
        //查询是否已经存在的实例
        for(index=1;index<=g_imusr_array.max_user_num;index++)
        {
            if(strcmp(user_id,g_imusr_array.usrnode[index].user_toxid) == OK)
            {
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"get idel index failed");
                break;
            }
        }
        if(index <= g_imusr_array.max_user_num)
        {
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
            pthread_cancel(g_imusr_array.usrnode[index].tox_tid);
            //清除对应记录
            ret_code = PNR_USER_DESTORY_OK;
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s)",router_id);
            ret_code = PNR_USER_DESTORY_BAD_USERID;
        }
    }
    
    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_DESTORY));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(user_id));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_addfriend_req_deal
  Description: IM模块添加好友消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_addfriend_req_deal(cJSON * params,char* retmsg,int* retmsg_len,
    int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_friend_msgstruct *msg;
    char retmsg_buff[PNR_USERNAME_MAXLEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;

    if(params == NULL)
    {
        return ERROR;
    }

    msg = (struct im_friend_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    head->forward = TRUE;
    
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",msg->touser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",msg->nickname,PNR_USERNAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserKey",msg->user_pubkey,PNR_USER_PUBKEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Msg",msg->friend_msg,PNR_FRIEND_MSG_MAXLEN);

    //useid 处理
    if(strlen(msg->fromuser_toxid) != TOX_ID_STR_LEN
        || strlen(msg->touser_toxid) != TOX_ID_STR_LEN
        || strlen(msg->user_pubkey) < DEFAULT_DES_KEYLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s->%s) userkey(%s)",
            msg->fromuser_toxid,msg->touser_toxid,msg->user_pubkey);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else if(strcmp(msg->fromuser_toxid,msg->touser_toxid) == OK)
    {
       DEBUG_PRINT(DEBUG_LEVEL_ERROR,"userid repeat(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid); 
       ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(msg->fromuser_toxid);
        if(index == 0)
        {
            //清除对应记录
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get fromuser_toxid(%s) failed",msg->fromuser_toxid);
        }
        else
        {
            if(*plws_index == 0)
            {
                *plws_index = index;
            }

			if (if_friend_available(index, msg->touser_toxid)) {
				ret_code = PNR_USER_ADDFRIEND_FRIEND_EXSIT;
			} else {
            	ret_code = PNR_USER_ADDFRIEND_RETOK;
			}
			
            if (head->iftox) {
                head->toxmsg = msg;
                head->im_cmdtype = PNR_IM_CMDTYPE_ADDFRIENDPUSH;
                head->to_userid = get_indexbytoxid(msg->touser_toxid);
            } else {
                i = get_indexbytoxid(msg->touser_toxid);
                if(i != 0)
                {
                    im_pushmsg_callback(i,PNR_IM_CMDTYPE_ADDFRIENDPUSH,TRUE,head->api_version,(void *)msg);
                }
                else
                {
                    im_pushmsg_callback(index,PNR_IM_CMDTYPE_ADDFRIENDPUSH,FALSE,head->api_version,(void *)msg);
                }
            }
        }
    }
    
    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_ADDFRIEDNREQ));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(retmsg_buff));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);

    if (!head->iftox)
        free(msg);
    
    return OK;

ERR:
    if (!head->iftox)
        free(msg);
    
    return ERROR;
}

/**********************************************************************************
  Function:      im_addfriend_deal_deal
  Description: IM模块添加好友处理结果解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_addfriend_deal_deal(cJSON * params,char* retmsg,int* retmsg_len,
    int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_friend_msgstruct *msg;
    char retmsg_buff[PNR_USERNAME_MAXLEN+1] = {0};
    char friend_pubkey[PNR_USER_PUBKEY_MAXLEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0;
    int friend_id = 0;

    if (!params) {
        return ERROR;
    }

    msg = (struct im_friend_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    head->forward = TRUE;
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",msg->nickname,PNR_USERNAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",msg->touser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendName",msg->friend_nickname,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserKey",msg->user_pubkey,PNR_USER_PUBKEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendKey",friend_pubkey,PNR_USER_PUBKEY_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Result",msg->result,TOX_ID_STR_LEN);
    if(head->api_version == PNR_API_VERSION_V4)
    {
        CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Sign",msg->sign,PNR_RSA_KEY_MAXLEN);
    }

    //查询是否已经存在的实例
    for(index=1;index<=g_imusr_array.max_user_num;index++)
    {
        if(strcmp(msg->fromuser_toxid,g_imusr_array.usrnode[index].user_toxid) == OK)
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get fromuser_toxid(%s)",msg->fromuser_toxid);
            break;
        }
    }
    //非本地实例消息
    if((index > g_imusr_array.max_user_num)
        ||((msg->result == OK) &&((strlen(friend_pubkey) < DEFAULT_DES_KEYLEN ) || (strlen(msg->user_pubkey) < DEFAULT_DES_KEYLEN))))
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_deal_deal:get index(%d) friend_pubkey(%s) user_pubkey(%s) error",
            index,friend_pubkey,msg->user_pubkey);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        if(*plws_index == 0)
        {
            *plws_index = index;
        }
        //如果添加成功，先添加本地数据库
        if(msg->result == OK)
        {
            pnr_friend_dbinsert(msg->fromuser_toxid,msg->touser_toxid,msg->friend_nickname,friend_pubkey);
            im_nodelist_addfriend(index,msg->fromuser_toxid,msg->touser_toxid,msg->friend_nickname,friend_pubkey);
            pnr_addfriend_devinfo_bytoxid(index,msg->touser_toxid);
        }
        ret_code = PNR_MSGSEND_RETCODE_OK;

        if (head->iftox) {
            head->toxmsg = msg;
            head->im_cmdtype = PNR_IM_CMDTYPE_ADDFRIENDREPLY;
            head->to_userid = get_indexbytoxid(msg->touser_toxid);
        } else {
            friend_id = get_indexbytoxid(msg->touser_toxid);
            if(friend_id != 0)
            {
                im_pushmsg_callback(friend_id,PNR_IM_CMDTYPE_ADDFRIENDREPLY,TRUE,head->api_version,(void *)msg);
            }
            else
            {
                im_pushmsg_callback(index,PNR_IM_CMDTYPE_ADDFRIENDREPLY,FALSE,head->api_version,(void *)msg);
            }
        }
    }
 
    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_ADDREIENDDEAL));
    cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(msg->touser_toxid));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(retmsg_buff));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);

    if (!head->iftox)
        free(msg);
    
    return OK;

ERR:
    if (!head->iftox)
        free(msg);
    
    return ERROR;
}

/**********************************************************************************
  Function:      im_delfriend_cmd_deal
  Description: IM模块删除好友消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_delfriend_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
    int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_friend_msgstruct *msg;
    char retmsg_buff[PNR_USERNAME_MAXLEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;

    if (!params) {
        return ERROR;
    }

    msg = (struct im_friend_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    head->forward = TRUE;

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",msg->touser_toxid,TOX_ID_STR_LEN);

    //useid 处理
    if(strlen(msg->fromuser_toxid) != TOX_ID_STR_LEN
        || strlen(msg->touser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(msg->fromuser_toxid);
        if(index == 0)
        {
            //清除对应记录
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get fromuser_toxid(%s) failed",msg->fromuser_toxid);
        }
        else
        {
            if(*plws_index == 0)
            {
                *plws_index = index;
            }

			int iffriend = if_friend_available(index, msg->touser_toxid);
			
			pnr_msgcache_dbdelete_by_friendid(index, msg->touser_toxid);
            im_nodelist_delfriend(index,msg->fromuser_toxid,msg->touser_toxid,0);
            pnr_friend_dbdelete(msg->fromuser_toxid,msg->touser_toxid,0);
            ret_code = PNR_USER_ADDFRIEND_RETOK;

			if (iffriend) {
	            if (head->iftox) {
	                head->toxmsg = msg;
	                head->im_cmdtype = PNR_IM_CMDTYPE_DELFRIENDPUSH;
	                head->to_userid = get_indexbytoxid(msg->touser_toxid);
	            } else {
	                i = get_indexbytoxid(msg->touser_toxid);
	                if(i != 0)
	                {
	                    im_pushmsg_callback(i,PNR_IM_CMDTYPE_DELFRIENDPUSH,TRUE,head->api_version,(void *)msg);
	                }
	                else
	                {
	                    im_pushmsg_callback(index,PNR_IM_CMDTYPE_DELFRIENDPUSH,FALSE,head->api_version,(void *)msg);
	                }
	            }
			} else {
				int friendnum = GetFriendNumInFriendlist_new(g_tox_linknode[index], msg->touser_toxid);
				if (friendnum >= 0) {
					tox_friend_delete(g_tox_linknode[index], friendnum, NULL);
				}
			}
        }
    }
    
    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_DELFRIENDCMD));
    cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(msg->touser_toxid));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(retmsg_buff));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);

    if (!head->iftox)
        free(msg);
    
    return OK;

ERR:
    if (!head->iftox)
        free(msg);
    
    return ERROR;
}

/**********************************************************************************
  Function:      im_sendmsg_cmd_deal
  Description: IM模块发送消息命令解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_sendmsg_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_sendmsg_msgstruct *msg;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
    
    if (!params) {
        return ERROR;
    }

    msg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FromId",msg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"ToId",msg->touser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Msg",msg->msg_buff,IM_MSG_PAYLOAD_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"SrcKey",msg->msg_srckey,PNR_RSA_KEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"DstKey",msg->msg_dstkey,PNR_RSA_KEY_MAXLEN);
    msg->msgtype = PNR_IM_MSGTYPE_TEXT;

	//useid 处理
    if(strlen(msg->fromuser_toxid) != TOX_ID_STR_LEN
        || strlen(msg->touser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else if(strcmp(msg->fromuser_toxid,msg->touser_toxid) == OK)
    {
       DEBUG_PRINT(DEBUG_LEVEL_ERROR,"userid repeat(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid); 
       ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else if(strlen(msg->msg_buff) > IM_MSG_PAYLOAD_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"msg too long(%d)",strlen(msg->msg_buff)); 
        ret_code = PNR_MSGSEND_RETCODE_FAILED;  
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(msg->fromuser_toxid);
        if(index == 0)
        {
            //清除对应记录
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get fromuser_toxid(%s) failed",msg->fromuser_toxid);
        }
        else
        {
            if(*plws_index == 0)
            {
                *plws_index = index;
            }

			if (!if_friend_available(index, msg->touser_toxid)) {
				ret_code = PNR_MSGSEND_RETCODE_NOT_FRIEND;
				goto OUT;
			}

            pnr_msglog_getid(index, &msg->log_id);
            pnr_msglog_dbupdate(index,msg->msgtype,msg->log_id,MSG_STATUS_SENDOK,msg->fromuser_toxid,
                msg->touser_toxid,msg->msg_buff,msg->msg_srckey,msg->msg_dstkey,NULL,0);

            ret_code = PNR_MSGSEND_RETCODE_OK;
			head->forward = TRUE;

            if (head->iftox) {
                head->toxmsg = msg;
                head->im_cmdtype = PNR_IM_CMDTYPE_PUSHMSG;
                head->to_userid = get_indexbytoxid(msg->touser_toxid);
            } else {
                i = get_indexbytoxid(msg->touser_toxid);
                if(i != 0)
                {
                    im_pushmsg_callback(i,PNR_IM_CMDTYPE_PUSHMSG,TRUE,head->api_version,(void *)msg);
                }
                else
                {
                    im_pushmsg_callback(index,PNR_IM_CMDTYPE_PUSHMSG,FALSE,head->api_version,(void *)msg);
                }
            }
        }
    }

OUT:
    //构建响应消息
	ret_root = cJSON_CreateObject();
    ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_SENDMSG));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msg->log_id));
    cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(msg->fromuser_toxid));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(msg->touser_toxid));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(msg->msg_buff));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);

    if (!head->iftox || !head->forward)
        free(msg);
    
    return OK;

ERR:
    if (!head->iftox || !head->forward)
        free(msg);
    
    return ERROR;
}
/**********************************************************************************
  Function:      im_sendmsg_cmd_deal_v3
  Description: IM模块发送消息命令V3版本解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_sendmsg_cmd_deal_v3(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_sendmsg_msgstruct *msg;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
    
    if (!params) {
        return ERROR;
    }

    msg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    //解析参数
    //暂时统一用toxid
#if 0    
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"From",msg->from_uid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"To",msg->to_uid,TOX_ID_STR_LEN);
#else
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"From",msg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"To",msg->touser_toxid,TOX_ID_STR_LEN);
#endif
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Msg",msg->msg_buff,IM_MSG_PAYLOAD_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Sign",msg->sign,PNR_RSA_KEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Nonce",msg->nonce,PNR_RSA_KEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"PriKey",msg->prikey,PNR_RSA_KEY_MAXLEN);
    msg->msgtype = PNR_IM_MSGTYPE_TEXT;

#if 0//暂时不用
	//useid 处理
	if(pnr_gettoxid_byhashid(msg->from_uid,msg->fromuser_toxid) != OK 
        || pnr_gettoxid_byhashid(msg->to_uid,msg->touser_toxid) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad index(%s->%s)",
            msg->from_uid,msg->to_uid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }   
    else
#endif        
    if(strlen(msg->fromuser_toxid) != TOX_ID_STR_LEN
        || strlen(msg->touser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else if(strcmp(msg->fromuser_toxid,msg->touser_toxid) == OK)
    {
       DEBUG_PRINT(DEBUG_LEVEL_ERROR,"userid repeat(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid); 
       ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else if(strlen(msg->msg_buff) > IM_MSG_PAYLOAD_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"msg too long(%d)",strlen(msg->msg_buff)); 
        ret_code = PNR_MSGSEND_RETCODE_FAILED;  
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(msg->fromuser_toxid);
        if(index == 0)
        {
            //清除对应记录
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get fromuser_toxid(%s) failed",msg->fromuser_toxid);
        }
        else
        {
            if(*plws_index == 0)
            {
                *plws_index = index;
            }

			if (!if_friend_available(index, msg->touser_toxid)) {
				ret_code = PNR_MSGSEND_RETCODE_NOT_FRIEND;
				goto OUT;
			}
			if (pnr_checkrepeat_bymsgid(index, head->msgid,&(head->repeat_flag)) == TRUE) {
				ret_code = PNR_MSGSEND_RETCODE_OK;
				goto OUT;
			}

            pnr_msglog_getid(index, &msg->log_id);
            pnr_msglog_dbupdate_v3(index,msg->msgtype,msg->log_id,MSG_STATUS_SENDOK,msg->fromuser_toxid,
                msg->touser_toxid,msg->msg_buff,msg->sign,msg->nonce,msg->prikey,NULL,0);

            ret_code = PNR_MSGSEND_RETCODE_OK;
			head->forward = TRUE;

            if (head->iftox) {
                head->toxmsg = msg;
                head->im_cmdtype = PNR_IM_CMDTYPE_PUSHMSG;
                head->to_userid = get_indexbytoxid(msg->touser_toxid);
            } else {
                i = get_indexbytoxid(msg->touser_toxid);
                if(i != 0)
                {
                    im_pushmsg_callback(i,PNR_IM_CMDTYPE_PUSHMSG,TRUE,head->api_version,(void *)msg);
                }
                else
                {
                    im_pushmsg_callback(index,PNR_IM_CMDTYPE_PUSHMSG,FALSE,head->api_version,(void *)msg);
                }
            }
        }
    }

OUT:
    //构建响应消息
	ret_root = cJSON_CreateObject();
    ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_SENDMSG));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msg->log_id));
#if 0
    cJSON_AddItemToObject(ret_params, "From", cJSON_CreateString(msg->from_uid));
    cJSON_AddItemToObject(ret_params, "To", cJSON_CreateString(msg->to_uid));
#else
    cJSON_AddItemToObject(ret_params, "From", cJSON_CreateString(msg->fromuser_toxid));
    cJSON_AddItemToObject(ret_params, "To", cJSON_CreateString(msg->touser_toxid));
#endif
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(msg->msg_buff));
    cJSON_AddItemToObject(ret_params, "Nonce", cJSON_CreateString(msg->nonce));
    cJSON_AddItemToObject(ret_params, "PriKey", cJSON_CreateString(msg->prikey));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    if(head->repeat_flag == TRUE)
    {
        cJSON_AddItemToObject(ret_params, "Repeat", cJSON_CreateNumber(TRUE));
    }

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);

    if (!head->iftox || !head->forward)
        free(msg);
    
    return OK;

ERR:
    if (!head->iftox || !head->forward)
        free(msg);
    
    return ERROR;
}

/**********************************************************************************
  Function:      im_delmsg_cmd_deal
  Description: IM模块撤回消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_delmsg_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_sendmsg_msgstruct *msg;
    char retmsg_buff[PNR_USERNAME_MAXLEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
	cJSON *ret_root =  NULL;
    cJSON *ret_params = NULL;

	if (!params) {
        return ERROR;
    }

    msg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }
    
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",msg->touser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgId",msg->log_id,TOX_ID_STR_LEN);
    msg->msgtype = PNR_IM_MSGTYPE_SYSTEM;

    if(strlen(msg->fromuser_toxid) != TOX_ID_STR_LEN
        || strlen(msg->touser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(msg->fromuser_toxid);
        if(index == 0)
        {
            //清除对应记录
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get fromuser_toxid(%s) failed",msg->fromuser_toxid);
        }
        else
        {
            if(*plws_index == 0)
            {
                *plws_index = index;
            }

			if (!if_friend_available(index, msg->touser_toxid)) {
				ret_code = PNR_MSGSEND_RETCODE_NOT_FRIEND;
				goto OUT;
			}

			pnr_msgcache_dbdelete_by_logid(index, msg);
            pnr_msglog_dbdelete(index,msg->msgtype,msg->log_id,msg->fromuser_toxid,msg->touser_toxid);
            ret_code = PNR_USER_ADDFRIEND_RETOK;
			head->forward = TRUE;

            if (head->iftox) {
                head->toxmsg = msg;
                head->im_cmdtype = PNR_IM_CMDTYPE_DELMSGPUSH;
                head->to_userid = get_indexbytoxid(msg->touser_toxid);
            } else {
                i = get_indexbytoxid(msg->touser_toxid);
                if(i != 0)
                {
                    im_pushmsg_callback(i,PNR_IM_CMDTYPE_DELMSGPUSH,TRUE,head->api_version,(void *)msg);
                }
                else
                {
                    im_pushmsg_callback(index,PNR_IM_CMDTYPE_DELMSGPUSH,FALSE,head->api_version,(void *)msg);
                }
            }
        }
    }

OUT:
    //构建响应消息
	ret_root =  cJSON_CreateObject();
    ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_DELMSG));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msg->log_id));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(retmsg_buff));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);

    if (!head->iftox || !head->forward)
        free(msg);
    
    return OK;

ERR:
    if (!head->iftox || !head->forward)
        free(msg);
    
    return ERROR;
}

/**********************************************************************************
  Function:      im_onlinestatus_check_deal
  Description: IM模块查询好友在线状态消息处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_onlinestatus_check_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_friend_msgstruct msg;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    int online_status = 0;
    char* ret_buff = NULL;
    int index = 0;

    if (!params) {
        return ERROR;
    }

    memset(&msg,0,sizeof(msg));

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg.fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"TargetUserId",msg.touser_toxid,TOX_ID_STR_LEN);

    //useid 处理
    if(strlen(msg.fromuser_toxid) != TOX_ID_STR_LEN
        || strlen(msg.touser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s->%s)",
            msg.fromuser_toxid,msg.touser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
        online_status = USER_ONLINE_STATUS_OFFLINE;
    }
    else
    {
        ret_code = PNR_MSGSEND_RETCODE_OK;
        //查询是否已经存在的实例
        index = get_indexbytoxid(msg.touser_toxid);
        if(index == 0)
        {
            //清除对应记录
            online_status = USER_ONLINE_STATUS_OFFLINE;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get UserId(%s) failed",msg.fromuser_toxid);
        }
        else
        {
            online_status = g_imusr_array.usrnode[index].user_onlinestatus;
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
        }
    }
    
    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_ONLINESTATUSCHECK));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "OnlineStatus", cJSON_CreateNumber(online_status));
    cJSON_AddItemToObject(ret_params, "TargetUserId", cJSON_CreateString(msg.touser_toxid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_heartbeat_cmd_deal
  Description: IM模块心跳命令解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_heartbeat_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_friend_msgstruct msg;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0;
    int active = PNR_APPACTIVE_STATUS_BUTT;
    if(params == NULL)
    {
        return ERROR;
    }

    memset(&msg,0,sizeof(msg));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg.fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Active",active,TOX_ID_STR_LEN);
    //useid 处理
    if(strlen(msg.fromuser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s)",
            msg.fromuser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        ret_code = PNR_MSGSEND_RETCODE_OK;
        //查询是否已经存在的实例
        index = get_indexbytoxid(msg.fromuser_toxid);
        if(index == 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get UserId(%s) failed",msg.fromuser_toxid);
        }
        else//心跳计数清零
        {            
            if(*plws_index == 0)
            {
                *plws_index = index;
            }   
            pthread_mutex_lock(&(g_pnruser_lock[index]));
            if(active == PNR_APPACTIVE_STATUS_FRONT || active == PNR_APPACTIVE_STATUS_BACKEND)
            {
                if(g_imusr_array.usrnode[index].appactive_flag != active)
                {
                    g_imusr_array.usrnode[index].appactive_flag = active;
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_heartbeat_cmd_deal:user(%d) appstatus change to %s",
                        index,(active == PNR_APPACTIVE_STATUS_FRONT?"front":"backend"));
                }
            }
            g_imusr_array.usrnode[index].heartbeat_count = 0;
            g_imusr_array.usrnode[index].user_onlinestatus = USER_ONLINE_STATUS_ONLINE;
            pthread_mutex_unlock(&(g_pnruser_lock[index]));
            /*DEBUG_PRINT(DEBUG_LEVEL_INFO,"hearbeat_deal: set user(%d) user_onlinestatus(%d) user_online_type(%d)",
                index,g_imusr_array.usrnode[index].user_onlinestatus,g_imusr_array.usrnode[index].user_online_type);*/
        }
    }
    
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_HEARTBEAT));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(""));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_pullmsg_cmd_deal
  Description: IM模块拉取聊天信息消息处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_pullmsg_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_sendmsg_msgstruct* pmsg = NULL;
    struct im_sendmsg_msgstruct* ptmp_msg = NULL;
    int msgnum = 0;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
    char **dbResult; 
    char *errmsg;
    int nRow, nColumn;
    int offset=0;
    char sql_cmd[SQL_CMD_LEN] = {0};
    if(params == NULL)
    {
        return ERROR;
    }
    pmsg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*pmsg));
    if (!pmsg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }
    ptmp_msg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*ptmp_msg));
    if (!ptmp_msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err2");
        free(pmsg);
        return ERROR;
    }

    //解析参数
    memset(pmsg,0,sizeof(struct im_sendmsg_msgstruct));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",pmsg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",pmsg->touser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgType",pmsg->msgtype,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgStartId",pmsg->log_id,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgNum",msgnum,TOX_ID_STR_LEN);

    //useid 处理
    if(msgnum <= 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad msgnum(%s)",msgnum);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else if(strlen(pmsg->fromuser_toxid) != TOX_ID_STR_LEN || strlen(pmsg->touser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s:%s)",pmsg->fromuser_toxid,pmsg->touser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(pmsg->fromuser_toxid);
        if(index == 0)
        {
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get UserId(%s) failed",pmsg->fromuser_toxid);
        }
        else
        {
            ret_code = PNR_MSGSEND_RETCODE_OK;
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
        }
    }
    
    //构建响应消息
    cJSON *ret_root = cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        free(pmsg);
        free(ptmp_msg);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
	cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLMSG));
#if 1
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(pmsg->fromuser_toxid));
    cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(pmsg->touser_toxid));
#endif
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    if(ret_code == PNR_MSGSEND_RETCODE_OK && index != 0)
    {
        if(msgnum > PNR_IMCMD_PULLMSG_MAXNUM)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"msgmun(%d) outof maxnum(%d)",msgnum,PNR_IMCMD_PULLMSG_MAXNUM);
            msgnum = PNR_IMCMD_PULLMSG_MAXNUM;
        }
#if (DB_CURRENT_VERSION < DB_VERSION_V3)
        if(pmsg->log_id == 0)
        {
            snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select id,logid,timestamp,status,"
				"from_user,to_user,msg,msgtype,ext,ext2,skey,dkey,id from msg_tbl where "
				"userindex=%d and ((from_user='%s' and to_user='%s') or "
				"(from_user='%s' and to_user='%s')) and msgtype not in (%d,%d) "
				"order by id desc limit %d)temp order by id;",
                index, pmsg->fromuser_toxid, pmsg->touser_toxid,
                pmsg->touser_toxid, pmsg->fromuser_toxid,
                PNR_IM_MSGTYPE_SYSTEM, PNR_IM_MSGTYPE_AVATAR, msgnum);
        }
        else
        {
            snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select id,logid,timestamp,status,"
				"from_user,to_user,msg,msgtype,ext,ext2,skey,dkey,id from msg_tbl where "
				"userindex=%d and id<%d and ((from_user='%s' and to_user='%s') or "
                "(from_user='%s' and to_user='%s')) and msgtype not in (%d,%d) "
                "order by id desc limit %d)temp order by id;",
                index,pmsg->log_id, pmsg->fromuser_toxid, pmsg->touser_toxid,
                pmsg->touser_toxid, pmsg->fromuser_toxid,
                PNR_IM_MSGTYPE_SYSTEM, PNR_IM_MSGTYPE_AVATAR, msgnum);
        }
#else
        if(pmsg->log_id == 0)
        {
            snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select id,logid,timestamp,status,"
				"from_user,to_user,msg,msgtype,ext,ext2,sign,prikey,id from msg_tbl where "
				"userindex=%d and ((from_user='%s' and to_user='%s') or "
				"(from_user='%s' and to_user='%s')) and msgtype not in (%d,%d) "
				"order by id desc limit %d)temp order by id;",
                index, pmsg->fromuser_toxid, pmsg->touser_toxid,
                pmsg->touser_toxid, pmsg->fromuser_toxid,
                PNR_IM_MSGTYPE_SYSTEM, PNR_IM_MSGTYPE_AVATAR, msgnum);
        }
        else
        {
            snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select id,logid,timestamp,status,"
				"from_user,to_user,msg,msgtype,ext,ext2,sign,prikey,id from msg_tbl where "
				"userindex=%d and id<%d and ((from_user='%s' and to_user='%s') or "
                "(from_user='%s' and to_user='%s')) and msgtype not in (%d,%d) "
                "order by id desc limit %d)temp order by id;",
                index,pmsg->log_id, pmsg->fromuser_toxid, pmsg->touser_toxid,
                pmsg->touser_toxid, pmsg->fromuser_toxid,
                PNR_IM_MSGTYPE_SYSTEM, PNR_IM_MSGTYPE_AVATAR, msgnum);
        }
#endif
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "sql_cmd(%s)",sql_cmd);
        if(sqlite3_get_table(g_msglogdb_handle[index], sql_cmd, &dbResult, &nRow, 
            &nColumn, &errmsg) == SQLITE_OK)
        {
            offset = nColumn; //字段值从offset开始呀
            for( i = 0; i < nRow ; i++ )
            {				
                memset(ptmp_msg,0,sizeof(struct im_sendmsg_msgstruct));
                ptmp_msg->db_id = atoi(dbResult[offset++]);
                ptmp_msg->log_id = atoi(dbResult[offset++]);
                ptmp_msg->timestamp = atoi(dbResult[offset++]);
                ptmp_msg->msg_status = atoi(dbResult[offset++]);
                snprintf(ptmp_msg->fromuser_toxid,TOX_ID_STR_LEN+1,"%s",dbResult[offset++]);
                snprintf(ptmp_msg->touser_toxid,TOX_ID_STR_LEN+1,"%s",dbResult[offset++]);
                snprintf(ptmp_msg->msg_buff,IM_MSG_PAYLOAD_MAXLEN,"%s",dbResult[offset++]);
				ptmp_msg->msgtype = atoi(dbResult[offset++]);
				snprintf(ptmp_msg->ext,IM_MSG_MAXLEN,"%s",dbResult[offset++]);
                ptmp_msg->ext2 = atoi(dbResult[offset++]);
				snprintf(ptmp_msg->msg_srckey,PNR_RSA_KEY_MAXLEN,"%s",dbResult[offset++]);
				snprintf(ptmp_msg->msg_dstkey,PNR_RSA_KEY_MAXLEN,"%s",dbResult[offset++]);
                //跳过id值
				offset ++;
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    free(pmsg);
                    free(ptmp_msg);
                    return ERROR;
                }
                cJSON_AddItemToArray(pJsonArry,pJsonsub); 
                cJSON_AddNumberToObject(pJsonsub,"MsgId",ptmp_msg->db_id); 
                //cJSON_AddNumberToObject(pJsonsub,"DbId",tmp_msg.db_id); 
                cJSON_AddNumberToObject(pJsonsub,"MsgType",ptmp_msg->msgtype); 
                cJSON_AddNumberToObject(pJsonsub,"TimeStamp",ptmp_msg->timestamp); 
#if 1
                if(strcmp(ptmp_msg->fromuser_toxid,pmsg->fromuser_toxid) == OK)
                {
                    cJSON_AddNumberToObject(pJsonsub,"Status",ptmp_msg->msg_status); 
                    cJSON_AddNumberToObject(pJsonsub,"Sender",USER_MSG_SENDER_SELF);
                    cJSON_AddStringToObject(pJsonsub,"UserKey",ptmp_msg->msg_srckey);
                }
                else
                {
                    cJSON_AddNumberToObject(pJsonsub,"Sender",USER_MSG_RECIVEVE_SELF);
                    cJSON_AddStringToObject(pJsonsub,"UserKey",ptmp_msg->msg_dstkey);
                }
#else
                cJSON_AddStringToObject(pJsonsub,"From",tmp_msg.fromuser_toxid);
                cJSON_AddStringToObject(pJsonsub,"To",tmp_msg.touser_toxid);
                if(strcmp(tmp_msg.fromuser_toxid,msg.fromuser_toxid) == OK)
                {
                    cJSON_AddStringToObject(pJsonsub,"UserKey",tmp_msg.msg_srckey);
                }
                else
                {
                    cJSON_AddStringToObject(pJsonsub,"UserKey",tmp_msg.msg_dstkey);
                }
#endif                
				/* need to return filepath */
				switch (ptmp_msg->msgtype) 
                {
    				case PNR_IM_MSGTYPE_FILE:
    				case PNR_IM_MSGTYPE_IMAGE:
    				case PNR_IM_MSGTYPE_AUDIO:
    				case PNR_IM_MSGTYPE_MEDIA:
    					cJSON_AddStringToObject(pJsonsub, "FileName", ptmp_msg->msg_buff);
    					cJSON_AddStringToObject(pJsonsub, "FilePath", ptmp_msg->ext);
                        cJSON_AddNumberToObject(pJsonsub, "FileSize", ptmp_msg->ext2);
    					break;

    				default:
    					cJSON_AddStringToObject(pJsonsub, "Msg", ptmp_msg->msg_buff);
				}
                /*DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_pullmsg_cmd_deal:id(%d) logid(%d)(%s->%s) Msg(%s)",
                    i,tmp_msg.log_id,tmp_msg.fromuser_toxid,tmp_msg.touser_toxid,tmp_msg.msg_buff);*/
            }
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"get nRow(%d) nColumn(%d)",nRow,nColumn);
            msgnum = i;
            sqlite3_free_table(dbResult);
        }
        cJSON_AddItemToObject(ret_params, "MsgNum", cJSON_CreateNumber(msgnum));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "MsgNum", cJSON_CreateNumber(0));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    free(pmsg);
    free(ptmp_msg);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < VERSION_MAXLEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_pullmsg_cmd_deal_v3
  Description: IM模块拉取聊天信息消息处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_pullmsg_cmd_deal_v3(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_sendmsg_msgstruct* pmsg = NULL;
    struct im_sendmsg_msgstruct* ptmp_msg = NULL;
    int msgnum = 0;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
    char **dbResult; 
    char *errmsg;
    int nRow, nColumn;
    int offset=0;
    char sql_cmd[SQL_CMD_LEN] = {0};
    char fileinfo[PNR_FILEINFO_MAXLEN+1] = {0};
    char filemd5[PNR_MD5_VALUE_MAXLEN+1] = {0};
    char filepath[PNR_FILEPATH_MAXLEN+1] = {0};
    char* ptmp = NULL;
    int fileinfo_len =0;

    if(params == NULL)
    {
        return ERROR;
    }
    pmsg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*pmsg));
    if (!pmsg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }
    ptmp_msg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*ptmp_msg));
    if (!ptmp_msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err2");
        free(pmsg);
        return ERROR;
    }

    //解析参数
    memset(pmsg,0,sizeof(struct im_sendmsg_msgstruct));
#if 0//暂时不用
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg.from_uid,PNR_USER_HASHID_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",msg.to_uid,PNR_USER_HASHID_MAXLEN);
#else
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",pmsg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",pmsg->touser_toxid,TOX_ID_STR_LEN);
#endif
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgType",pmsg->msgtype,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgStartId",pmsg->log_id,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgNum",msgnum,TOX_ID_STR_LEN);

    //useid 处理
    if(msgnum <= 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad msgnum(%s)",msgnum);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
#if 0 //暂时不用
    else if((pnr_gettoxid_byhashid(msg.from_uid,msg.fromuser_toxid) != OK)
        ||(pnr_gettoxid_byhashid(msg.to_uid,msg.touser_toxid) != OK))
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_gettoxid_byhashid err");
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
#endif
    else if(strlen(pmsg->fromuser_toxid) != TOX_ID_STR_LEN || strlen(pmsg->touser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s:%s)",pmsg->fromuser_toxid,pmsg->touser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(pmsg->fromuser_toxid);
        if(index == 0)
        {
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get UserId(%s) failed",pmsg->fromuser_toxid);
        }
        else
        {
            ret_code = PNR_MSGSEND_RETCODE_OK;
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
        }
    }
    
    //构建响应消息
    cJSON *ret_root = cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        free(pmsg);
        free(ptmp_msg);
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        free(pmsg);
        free(ptmp_msg);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
	cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLMSG));
#if 0//暂时不用
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(msg.from_uid));
    cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(msg.to_uid));
#else
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(pmsg->fromuser_toxid));
    cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(pmsg->touser_toxid));
#endif
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    if(ret_code == PNR_MSGSEND_RETCODE_OK && index != 0)
    {
        if(msgnum > PNR_IMCMD_PULLMSG_MAXNUM)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"msgmun(%d) outof maxnum(%d)",msgnum,PNR_IMCMD_PULLMSG_MAXNUM);
            msgnum = PNR_IMCMD_PULLMSG_MAXNUM;
        }
        if(pmsg->log_id == 0)
        {
            snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select id,logid,timestamp,status,"
				"from_user,to_user,msg,msgtype,ext,ext2,sign,nonce,prikey,id from msg_tbl where "
				"userindex=%d and ((from_user='%s' and to_user='%s') or "
				"(from_user='%s' and to_user='%s')) and msgtype not in (%d,%d) "
				"order by id desc limit %d)temp order by id;",
                index, pmsg->fromuser_toxid, pmsg->touser_toxid,
                pmsg->touser_toxid, pmsg->fromuser_toxid,
                PNR_IM_MSGTYPE_SYSTEM, PNR_IM_MSGTYPE_AVATAR, msgnum);
        }
        else
        {
            snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select id,logid,timestamp,status,"
				"from_user,to_user,msg,msgtype,ext,ext2,sign,nonce,prikey,id from msg_tbl where "
				"userindex=%d and id<%d and ((from_user='%s' and to_user='%s') or "
                "(from_user='%s' and to_user='%s')) and msgtype not in (%d,%d) "
                "order by id desc limit %d)temp order by id;",
                index,pmsg->log_id, pmsg->fromuser_toxid, pmsg->touser_toxid,
                pmsg->touser_toxid, pmsg->fromuser_toxid,
                PNR_IM_MSGTYPE_SYSTEM, PNR_IM_MSGTYPE_AVATAR, msgnum);
        }

        DEBUG_PRINT(DEBUG_LEVEL_INFO, "sql_cmd(%s)",sql_cmd);
        if(sqlite3_get_table(g_msglogdb_handle[index], sql_cmd, &dbResult, &nRow, 
            &nColumn, &errmsg) == SQLITE_OK)
        {
            offset = nColumn; //字段值从offset开始呀
            for( i = 0; i < nRow ; i++ )
            {				
                memset(ptmp_msg,0,sizeof(struct im_sendmsg_msgstruct));
                ptmp_msg->db_id = atoi(dbResult[offset++]);
                ptmp_msg->log_id = atoi(dbResult[offset++]);
                ptmp_msg->timestamp = atoi(dbResult[offset++]);
                ptmp_msg->msg_status = atoi(dbResult[offset++]);
                snprintf(ptmp_msg->fromuser_toxid,TOX_ID_STR_LEN+1,"%s",dbResult[offset++]);
                snprintf(ptmp_msg->touser_toxid,TOX_ID_STR_LEN+1,"%s",dbResult[offset++]);
                snprintf(ptmp_msg->msg_buff,IM_MSG_PAYLOAD_MAXLEN,"%s",dbResult[offset++]);
				ptmp_msg->msgtype = atoi(dbResult[offset++]);
				snprintf(ptmp_msg->ext,IM_MSG_MAXLEN,"%s",dbResult[offset++]);
                ptmp_msg->ext2 = atoi(dbResult[offset++]);
                snprintf(ptmp_msg->sign,PNR_RSA_KEY_MAXLEN,"%s",dbResult[offset++]);
                snprintf(ptmp_msg->nonce,PNR_RSA_KEY_MAXLEN,"%s",dbResult[offset++]);
                snprintf(ptmp_msg->prikey,PNR_RSA_KEY_MAXLEN,"%s",dbResult[offset++]);
                //跳过id值
				offset ++;
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    return ERROR;
                }
                cJSON_AddItemToArray(pJsonArry,pJsonsub); 
                cJSON_AddNumberToObject(pJsonsub,"MsgId",ptmp_msg->db_id); 
                //cJSON_AddNumberToObject(pJsonsub,"DbId",ptmp_msg->db_id); 
                cJSON_AddNumberToObject(pJsonsub,"MsgType",ptmp_msg->msgtype); 
                cJSON_AddNumberToObject(pJsonsub,"TimeStamp",ptmp_msg->timestamp); 
                cJSON_AddNumberToObject(pJsonsub,"Status",ptmp_msg->msg_status); 
#if 1
                if(strcmp(ptmp_msg->fromuser_toxid,pmsg->fromuser_toxid) == OK)
                {
                    cJSON_AddNumberToObject(pJsonsub,"Sender",USER_MSG_SENDER_SELF);
                }
                else
                {
                    cJSON_AddNumberToObject(pJsonsub,"Sender",USER_MSG_RECIVEVE_SELF);
                }
                cJSON_AddStringToObject(pJsonsub,"Nonce",ptmp_msg->nonce);
                cJSON_AddStringToObject(pJsonsub,"Sign",ptmp_msg->sign);
                cJSON_AddStringToObject(pJsonsub,"PriKey",ptmp_msg->prikey);
#else
                cJSON_AddStringToObject(pJsonsub,"From",tmp_msg.fromuser_toxid);
                cJSON_AddStringToObject(pJsonsub,"To",tmp_msg.touser_toxid);
                if(strcmp(tmp_msg.fromuser_toxid,msg.fromuser_toxid) == OK)
                {
                    cJSON_AddStringToObject(pJsonsub,"UserKey",tmp_msg.msg_srckey);
                }
                else
                {
                    cJSON_AddStringToObject(pJsonsub,"UserKey",tmp_msg.msg_dstkey);
                }
#endif                
				/* need to return filepath */
				switch (ptmp_msg->msgtype) 
                {
    				case PNR_IM_MSGTYPE_FILE:
    				case PNR_IM_MSGTYPE_IMAGE:
    				case PNR_IM_MSGTYPE_AUDIO:
    				case PNR_IM_MSGTYPE_MEDIA:
    					cJSON_AddStringToObject(pJsonsub, "FileName", ptmp_msg->msg_buff);
                        ptmp = strchr(ptmp_msg->ext,PNR_FILEINFO_ATTACH_FLAG);
                        if(ptmp != NULL)
                        {
                            fileinfo_len = ptmp_msg->ext+strlen(ptmp_msg->ext)-ptmp;
                            fileinfo_len = ((fileinfo_len >= PNR_FILEINFO_MAXLEN)?PNR_FILEINFO_MAXLEN:fileinfo_len);
                            memset(fileinfo,0,PNR_FILEINFO_MAXLEN);
                            strncpy(fileinfo,ptmp,fileinfo_len);
                            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"####get ext(%s) fileinfo(%s)",tmp_msg.ext,fileinfo);
                            ptmp[0] = 0;
                            cJSON_AddStringToObject(pJsonsub, "FileInfo", fileinfo+1);
                        }
                        if(strncmp(ptmp_msg->ext,WS_SERVER_INDEX_FILEPATH,strlen(WS_SERVER_INDEX_FILEPATH)) == OK)
                        {
                            cJSON_AddStringToObject(pJsonsub, "FilePath", ptmp_msg->ext+strlen(WS_SERVER_INDEX_FILEPATH));
                            md5_hash_file(ptmp_msg->ext, filemd5);
                            cJSON_AddStringToObject(pJsonsub, "FileMD5", filemd5);
                        }
                        else
                        {
                            snprintf(filepath,PNR_FILEPATH_MAXLEN,"%s/%s",WS_SERVER_INDEX_FILEPATH,ptmp_msg->ext);
                            md5_hash_file(filepath, filemd5);
                            cJSON_AddStringToObject(pJsonsub, "FileMD5", filemd5);
                            cJSON_AddStringToObject(pJsonsub, "FilePath", ptmp_msg->ext);
                        }
                        cJSON_AddNumberToObject(pJsonsub, "FileSize", ptmp_msg->ext2);
    					break;

    				default:
    					cJSON_AddStringToObject(pJsonsub, "Msg", ptmp_msg->msg_buff);
				}
                /*DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_pullmsg_cmd_deal:id(%d) logid(%d)(%s->%s) Msg(%s)",
                    i,tmp_msg.log_id,tmp_msg.fromuser_toxid,tmp_msg.touser_toxid,tmp_msg.msg_buff);*/
            }
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"get nRow(%d) nColumn(%d)",nRow,nColumn);
            msgnum = i;
            sqlite3_free_table(dbResult);
        }
        cJSON_AddItemToObject(ret_params, "MsgNum", cJSON_CreateNumber(msgnum));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "MsgNum", cJSON_CreateNumber(0));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    free(pmsg);
    free(ptmp_msg);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < VERSION_MAXLEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_pullfriend_cmd_deal
  Description: IM模块拉取好友信息消息处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_pullfriend_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char user_toxid[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_toxid,TOX_ID_STR_LEN);

    //useid 处理
    if(strlen(user_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s)",user_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(user_toxid);
        if(index == 0)
        {
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get UserId(%s) failed",user_toxid);
        }
        else
        {
            ret_code = PNR_MSGSEND_RETCODE_OK;
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
        }
    }
    
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

	cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLFRIEDN));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    if(ret_code == PNR_MSGSEND_RETCODE_OK && index != 0
        && g_imusr_array.usrnode[index].friendnum > 0)
    {
        pthread_mutex_lock(&(g_user_friendlock[index]));
        cJSON_AddItemToObject(ret_params, "FriendNum", cJSON_CreateNumber(g_imusr_array.usrnode[index].friendnum));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
        for(i=0;i<PNR_IMUSER_FRIENDS_MAXNUM;i++)
        {
            if(g_imusr_array.usrnode[index].friends[i].exsit_flag)
            {
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    pthread_mutex_unlock(&(g_user_friendlock[index]));
                    return ERROR;
                }
           		cJSON_AddItemToArray(pJsonArry,pJsonsub); 
        		cJSON_AddStringToObject(pJsonsub,"Name", g_imusr_array.usrnode[index].friends[i].user_nickname);
        		cJSON_AddStringToObject(pJsonsub,"Remarks", g_imusr_array.usrnode[index].friends[i].user_remarks);
        		cJSON_AddStringToObject(pJsonsub,"Id", g_imusr_array.usrnode[index].friends[i].user_toxid);
        		cJSON_AddStringToObject(pJsonsub,"UserKey", g_imusr_array.usrnode[index].friends[i].user_pubkey);
        		cJSON_AddNumberToObject(pJsonsub,"Status",g_imusr_array.usrnode[index].friends[i].online_status); 
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"get friend(%d:%s:%s:%s)",
                    i,g_imusr_array.usrnode[index].friends[i].user_nickname,
                    g_imusr_array.usrnode[index].friends[i].user_remarks,
                    g_imusr_array.usrnode[index].friends[i].user_toxid);
            }
        }
        pthread_mutex_unlock(&(g_user_friendlock[index]));
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "FriendNum", cJSON_CreateNumber(0));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < VERSION_MAXLEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_pullfriend_cmd_deal_v3
  Description: IM模块拉取好友信息消息处理V3版本
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_pullfriend_cmd_deal_v3(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char user_toxid[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_toxid,TOX_ID_STR_LEN);

    //useid 处理
    if(strlen(user_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s)",user_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(user_toxid);
        if(index == 0)
        {
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get UserId(%s) failed",user_toxid);
        }
        else
        {
            ret_code = PNR_MSGSEND_RETCODE_OK;
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
        }
    }
    
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

	cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLFRIEDN));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    if(ret_code == PNR_MSGSEND_RETCODE_OK && index != 0
        && g_imusr_array.usrnode[index].friendnum > 0)
    {
        pthread_mutex_lock(&(g_user_friendlock[index]));
        cJSON_AddItemToObject(ret_params, "FriendNum", cJSON_CreateNumber(g_imusr_array.usrnode[index].friendnum));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
        for(i=0;i<PNR_IMUSER_FRIENDS_MAXNUM;i++)
        {
            if(g_imusr_array.usrnode[index].friends[i].exsit_flag)
            {
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    pthread_mutex_unlock(&(g_user_friendlock[index]));
                    return ERROR;
                }
           		cJSON_AddItemToArray(pJsonArry,pJsonsub); 
#if 0//暂时不用
        		cJSON_AddStringToObject(pJsonsub,"Index", g_imusr_array.usrnode[index].friends[i].u_hashstr);
#endif
                cJSON_AddStringToObject(pJsonsub,"Name", g_imusr_array.usrnode[index].friends[i].user_nickname);
        		cJSON_AddStringToObject(pJsonsub,"Remarks", g_imusr_array.usrnode[index].friends[i].user_remarks);
        		cJSON_AddStringToObject(pJsonsub,"Id", g_imusr_array.usrnode[index].friends[i].user_toxid);
        		cJSON_AddStringToObject(pJsonsub,"UserKey", g_imusr_array.usrnode[index].friends[i].user_pubkey);
        		cJSON_AddNumberToObject(pJsonsub,"Status",g_imusr_array.usrnode[index].friends[i].online_status); 
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"get friend(%d:%s:%s:%s)",
                    i,g_imusr_array.usrnode[index].friends[i].user_nickname,
                    g_imusr_array.usrnode[index].friends[i].user_remarks,
                    g_imusr_array.usrnode[index].friends[i].user_toxid);
            }
        }
        pthread_mutex_unlock(&(g_user_friendlock[index]));
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "FriendNum", cJSON_CreateNumber(0));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < VERSION_MAXLEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_sysch_datafile_deal
  Description: IM模块data文件同步消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_sysch_datafile_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char user_id[TOX_ID_STR_LEN+1] = {0};
    char dst_file[PNR_FILEPATH_MAXLEN+1] = {0};
    char base64_buf[DATAFILE_BASE64_ENCODE_MAXLEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int data_version = 0;
    int ret_code = PNR_MSGSEND_RETCODE_OK;
    int synch_flag = 0;
    char* ret_buff = NULL;
    int index = 0,bufflen = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_id,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"NeedSynch",synch_flag,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserDataVersion",data_version,TOX_ID_STR_LEN);

    index = get_indexbytoxid(user_id);
    if(index == 0)
    {
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        snprintf(dst_file,PNR_FILEPATH_MAXLEN,"%s/user%d/%s",DAEMON_PNR_USERDATA_DIR,index,PNR_DATAFILE_DEFNAME);
        switch(synch_flag)
        {
            //用户侧上传data文件
            case PNR_IM_SYSCHDATAFILE_UPLOAD:
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"DataPay",base64_buf,DATAFILE_BASE64_ENCODE_MAXLEN);
                bufflen = strlen(base64_buf) - 1;
                if(pnr_datafile_base64decode(dst_file,base64_buf,bufflen) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_sysch_datafile_deal:pnr_datafile_base64decode failed");
                    ret_code = PNR_MSGSEND_RETCODE_FAILED;
                }
                else
                {
                    ret_code = PNR_MSGSEND_RETCODE_OK;
                }
                //这里考虑需不需要重启tox实例
                break;
            //用户从路由器上下载data文件
            case PNR_IM_SYSCHDATAFILE_DOWNLOAD:
                if(pnr_datafile_base64encode(dst_file,base64_buf,&bufflen) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_sysch_datafile_deal:pnr_datafile_base64encode failed");
                    ret_code = PNR_MSGSEND_RETCODE_FAILED;
                }   
                else
                {
                    ret_code = PNR_MSGSEND_RETCODE_OK;
                }
                break;
            case PNR_IM_SYSCHDATAFILE_NONEED:
            default:
                ret_code = PNR_MSGSEND_RETCODE_FAILED;
                break;
        }
    }

    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_SYNCHDATAFILE));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "NeedSynch", cJSON_CreateNumber(synch_flag));
    cJSON_AddItemToObject(ret_params, "UserDataVersion", cJSON_CreateNumber(data_version));
    if(ret_code == PNR_MSGSEND_RETCODE_OK && synch_flag == PNR_IM_SYSCHDATAFILE_DOWNLOAD)
    {
        cJSON_AddItemToObject(ret_params, "DataPay", cJSON_CreateString(base64_buf));
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "DataPay", cJSON_CreateString(""));
    }

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/*****************************************************************************
 函 数 名  : im_pull_file_list_deal
 功能描述  : 拉取文件列表
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月9日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_pull_file_list_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	char user_id[TOX_ID_STR_LEN + 1] = {0};
	char sql[1500] = {0};
	char category_str[256] = {0};
	char *tmp_json_buff = NULL;
	char *ret_buff = NULL;
    cJSON *tmp_item = NULL;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
	cJSON *ret_payloads = NULL;
	int category = 0, filetype = 0, num = -1, start = 0;
	int index = 0;
	int ret_code = 0, ret_num = 0;
    int filefrom = 0;
    struct pnr_account_struct src_account;
    if (!params) {
        return ERROR;
    }

    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "UserId", user_id, TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "Category", category, 0);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileType", filetype, 0);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "MsgStartId", start, 0);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "MsgNum", num, 0);

    index = get_indexbytoxid(user_id);
    if (index == 0) {
        ret_code = 1;
		goto OUT;
    }
	snprintf(sql, sizeof(sql), "select id,timestamp,msgtype,msg,ext,ext2,from_user,to_user,sign,prikey "
		"from msg_tbl where ");
	switch (category) {
	case PNR_FILE_ALL:
		snprintf(category_str, sizeof(category_str), "(from_user='%s' or to_user='%s')", user_id,user_id);
		break;

	case PNR_FILE_SEND:
		snprintf(category_str, sizeof(category_str), "(from_user='%s' and to_user!='')", user_id);
		break;

	case PNR_FILE_RECV:
		snprintf(category_str, sizeof(category_str), "(to_user='%s' and from_user!='')", user_id);
		break;

	case PNR_FILE_UPLOAD:
		snprintf(category_str, sizeof(category_str), "(from_user='%s' and to_user='')", user_id);
		break;

	default:
		ret_code = 2;
		goto OUT;
	}

	
	snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " %s", category_str);
	if (filetype) {
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " and msgtype=%d", filetype);
	} else {
	    //这里需要显示所有消息传输的文件类型(msgtype为5)以及自己上传的图片，视频，文件
#if 0
        snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " and msgtype in(%d,%d,%d)",PNR_IM_MSGTYPE_IMAGE, PNR_IM_MSGTYPE_MEDIA, PNR_IM_MSGTYPE_FILE);
#else
        snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " and (msgtype=%d or (msgtype in(%d,%d,%d) and ext like '%%/u/%%'))",
		    PNR_IM_MSGTYPE_FILE,PNR_IM_MSGTYPE_IMAGE, PNR_IM_MSGTYPE_MEDIA, PNR_IM_MSGTYPE_FILE);
#endif
	}

	if (start) {
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " and id<%d", start);
	}
	
	snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " group by ext order by id desc");

	if (num) {
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " limit %d;", num);
	}

	ret_payloads = cJSON_CreateArray();
    if (!ret_payloads) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_payloads);
        return ERROR;
    }

	char **dbResult; 
    char *errmsg;
    int nRow, nColumn;
    int offset = 0, i = 0;
    char filename[256] = {0};
    char* p_file = NULL;
	char filepath[512] = {0};
	char md5[33] = {0};
	char remark[128] = {0};
    char from_user[128]= {0};
    char to_user[128]= {0};
    int friendid = 0;
    char fileinfo[PNR_FILEINFO_MAXLEN+1] = {0};
    char* ptmp = NULL;
    int fileinfo_len =0;

	DEBUG_PRINT(DEBUG_LEVEL_INFO, "pull filelist(%s)", sql);
    if (sqlite3_get_table(g_msglogdb_handle[index], sql, &dbResult, &nRow, &nColumn, &errmsg) == SQLITE_OK) {
        offset = nColumn;

		for (i = 0; i < nRow ; i++) {
			cJSON *array_item = cJSON_CreateObject();
            if (!array_item) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "create json err");
				break;
            }
			
            cJSON_AddItemToArray(ret_payloads, array_item); 
			cJSON_AddItemToObject(array_item, "MsgId", cJSON_CreateNumber(atoi(dbResult[offset])));
			cJSON_AddItemToObject(array_item, "Timestamp", cJSON_CreateNumber(atoi(dbResult[offset+1])));
			cJSON_AddItemToObject(array_item, "FileType", cJSON_CreateNumber(atoi(dbResult[offset+2])));
            if(dbResult[offset+6] != NULL && dbResult[offset+4] != NULL)
            {
                fileinfo_len = 0;
                strcpy(filename,dbResult[offset+4]);
                ptmp = strchr(filename,PNR_FILEINFO_ATTACH_FLAG);
                if(ptmp != NULL)
                {
                    fileinfo_len = filename+strlen(filename)-ptmp;
                    fileinfo_len = ((fileinfo_len >= PNR_FILEINFO_MAXLEN)?PNR_FILEINFO_MAXLEN:fileinfo_len);
                    memset(fileinfo,0,PNR_FILEINFO_MAXLEN);
                    strncpy(fileinfo,ptmp,fileinfo_len);
                    ptmp[0] = 0;
                }
                strcpy(from_user,dbResult[offset+6]);
                if(category == PNR_FILE_ALL)
                {
                    if(strcasecmp(from_user,user_id) == OK)
                    {
                        if(strstr(filename,"/u/") != NULL)
                        {
                            filefrom = PNR_FILE_UPLOAD;
                        }
                        else
                        {
                            filefrom = PNR_FILE_SEND;
                        }
                    }
                    else
                    {
                        filefrom = PNR_FILE_RECV;
                    }
                }
                else if(category == PNR_FILE_SEND)
                {
                    if(strstr(filename,"/u/") != NULL)
                    {
                        filefrom = PNR_FILE_UPLOAD;
                    }
                    else
                    {
                        filefrom = PNR_FILE_SEND;
                    }
                }
                else if(category == PNR_FILE_RECV)
                {
                    filefrom = PNR_FILE_RECV;
                }
                else if(category == PNR_FILE_UPLOAD)
                {
                    filefrom = PNR_FILE_UPLOAD;
                }
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad from userid or filename");
				break;
            }
            if(strncmp(filename,WS_SERVER_INDEX_FILEPATH,strlen(WS_SERVER_INDEX_FILEPATH)) == OK)
            {
                p_file = filename+strlen(WS_SERVER_INDEX_FILEPATH);
            }
            else
            {
                p_file = filename;
            }

            if(head->api_version >= PNR_API_VERSION_V5)
            {
                cJSON_AddItemToObject(array_item, "FileName", cJSON_CreateString(dbResult[offset+3]));
                cJSON_AddItemToObject(array_item, "FilePath", cJSON_CreateString(p_file));
            }
            else
            {
                cJSON_AddItemToObject(array_item, "FileName", cJSON_CreateString(p_file));
            }
			snprintf(filepath, sizeof(filepath), WS_SERVER_INDEX_FILEPATH"%s", p_file);
			md5_hash_file(filepath, md5);
			cJSON_AddItemToObject(array_item, "FileMD5", cJSON_CreateString(md5));
            if(fileinfo_len > 0)
            {
                cJSON_AddItemToObject(array_item, "FileInfo", cJSON_CreateString(fileinfo+1));
            }
            cJSON_AddItemToObject(array_item, "FileSize", cJSON_CreateNumber(atoi(dbResult[offset+5])));
			if (filefrom == PNR_FILE_RECV) {
				memset(remark, 0, sizeof(remark));
				pnr_friend_get_remark(user_id, from_user, remark, sizeof(remark) - 1);
				cJSON_AddItemToObject(array_item, "Sender", cJSON_CreateString(remark));
				cJSON_AddItemToObject(array_item, "UserKey", cJSON_CreateString(dbResult[offset+9]));
                friendid = get_friendid_bytoxid(index,from_user);
                if(friendid)
                {
                    cJSON_AddItemToObject(array_item, "SenderKey", cJSON_CreateString(g_imusr_array.usrnode[index].friends[friendid].user_pubkey));
                }
                else
                {
                    memset(&src_account,0,sizeof(src_account));
                    strcpy(src_account.toxid,from_user);
                    pnr_account_dbget_byuserid(&src_account);
                    if(src_account.dbid != 0)
                    {
                        cJSON_AddItemToObject(array_item, "SenderKey", cJSON_CreateString(src_account.user_pubkey));
                    }
                }
            } 
            else 
            {
				cJSON_AddItemToObject(array_item, "UserKey", cJSON_CreateString(dbResult[offset+8]));
                if(filefrom == PNR_FILE_SEND)
                {
                    if(dbResult[offset+7])
                    {
                        memset(to_user,0,128);
                        strcpy(to_user,dbResult[offset+7]);
                        memset(remark, 0, sizeof(remark));
        				pnr_friend_get_remark(user_id, to_user, remark, sizeof(remark) - 1);
				        cJSON_AddItemToObject(array_item, "Sender", cJSON_CreateString(remark));
                    }
                    friendid = get_friendid_bytoxid(index,to_user);
                    if(friendid)
                    {
                        cJSON_AddItemToObject(array_item, "SenderKey", cJSON_CreateString(g_imusr_array.usrnode[index].friends[friendid].user_pubkey));
                    }
                    else
                    {
                        memset(&src_account,0,sizeof(src_account));
                        strcpy(src_account.toxid,to_user);
                        pnr_account_dbget_byuserid(&src_account);
                        if(src_account.dbid != 0)
                        {
                            cJSON_AddItemToObject(array_item, "SenderKey", cJSON_CreateString(src_account.user_pubkey));
                        }
                    }
                }
                else
                {
                    cJSON_AddItemToObject(array_item, "Sender", cJSON_CreateString(g_imusr_array.usrnode[index].user_nickname));
                    memset(&src_account,0,sizeof(src_account));
                    strcpy(src_account.toxid,g_imusr_array.usrnode[index].user_toxid);
                    pnr_account_dbget_byuserid(&src_account);                    
                    if(src_account.dbid != 0)
                    {
                        cJSON_AddItemToObject(array_item, "SenderKey", cJSON_CreateString(src_account.user_pubkey));
                    }
                }
			}
            cJSON_AddItemToObject(array_item, "FileFrom", cJSON_CreateNumber(filefrom));
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"file(%s) filefrom(%d)",filepath,filefrom);
			offset += nColumn;
        }

        ret_num = i;
        sqlite3_free_table(dbResult);
    } else {
    	DEBUG_PRINT(DEBUG_LEVEL_ERROR, "sql(%s) err(%s)", sql, errmsg);
		sqlite3_free(errmsg);
	}
	
OUT:
	ret_root = cJSON_CreateObject();
	if (!ret_root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_root);
		cJSON_Delete(ret_payloads);
        return ERROR;
    }
	
    ret_params = cJSON_CreateObject();
	if (!ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_params);
		cJSON_Delete(ret_payloads);
        return ERROR;
    }
		
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLFILELIST));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "FileNum", cJSON_CreateNumber(ret_num));

	if (ret_num > 0) {
		cJSON_AddItemToObject(ret_params, "Payload", ret_payloads);
	}
	
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
	
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad ret(%d)", *retmsg_len);
        free(ret_buff);
        return ERROR;
    }
	
    strcpy(retmsg, ret_buff);
    free(ret_buff);
	
    return OK;
}

/*****************************************************************************
 函 数 名  : im_upload_file_req_deal
 功能描述  : 上传文件请求
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月15日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_upload_file_req_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	char user_id[TOX_ID_STR_LEN + 1] = {0};
	char sql[1024] = {0};
	char filename[PNR_FILENAME_MAXLEN + 1] = {0};
	char *tmp_json_buff = NULL;
	char *ret_buff = NULL;
    cJSON *tmp_item = NULL;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
	int index = 0;
	int ret_code = 0, filesize = 0, filetype = 0;
	
    if (!params) {
        return ERROR;
    }

    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "UserId", user_id, TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileName", filename, PNR_FILENAME_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileSize", filesize, 0);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileType", filetype, 0);

    index = get_indexbytoxid(user_id);
    if (index == 0) {
        ret_code = 3;
		goto OUT;
    }

	snprintf(sql, sizeof(sql), "select id from msg_tbl where from_user='%s' and to_user='' and msg='%s';", 
		user_id, filename);

	char **dbResult; 
    char *errmsg;
    int nRow, nColumn;
		
    if (sqlite3_get_table(g_msglogdb_handle[index], sql, &dbResult, &nRow, &nColumn, &errmsg) == SQLITE_OK) {
		if (nRow > 0)
			ret_code = 1;
		
        sqlite3_free_table(dbResult);
    } else {
    	DEBUG_PRINT(DEBUG_LEVEL_ERROR, "sql(%s) err(%s)", sql, errmsg);
		sqlite3_free(errmsg);
		ret_code = 3;
	}

OUT:
	ret_root = cJSON_CreateObject();
	if (!ret_root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
	
    ret_params = cJSON_CreateObject();
	if (!ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_params);
        return ERROR;
    }
		
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_UPLOADFILEREQ));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
	
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad ret(%d)", *retmsg_len);
        free(ret_buff);
        return ERROR;
    }
	
    strcpy(retmsg, ret_buff);
    free(ret_buff);
	
	return OK;
}

/*****************************************************************************
 函 数 名  : im_upload_file_deal
 功能描述  : tox方式下调用该接口上报上传的文件的信息
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月15日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_upload_file_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	char user_id[TOX_ID_STR_LEN + 1] = {0};
	char filename[PNR_FILENAME_MAXLEN + 1] = {0};
	char filepath[PNR_FILEPATH_MAXLEN + 1] = {0};
	char fileinfo[PNR_USERNAME_MAXLEN + 1] = {0};
	char filemd5[33] = {0};
	char md5[33] = {0};
	char userkey[PNR_RSA_KEY_MAXLEN + 1] = {0};
	char *tmp_json_buff = NULL;
	char *ret_buff = NULL;
    cJSON *tmp_item = NULL;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
	int index = 0;
	int ret_code = 0, filesize = 0, filetype = 0;
	
    if (!params) {
        return ERROR;
    }

    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "UserId", user_id, TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileName", filename, PNR_FILENAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileMD5", filemd5, PNR_MD5_VALUE_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileInfo", fileinfo, PNR_USERNAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "UserKey", userkey, PNR_RSA_KEY_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileSize", filesize, 0);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileType", filetype, 0);

    index = get_indexbytoxid(user_id);
    if (index == 0) {
        ret_code = 3;
		goto OUT;
    }

	snprintf(filepath, sizeof(filepath), WS_SERVER_INDEX_FILEPATH"/user%d/u/%s", index, filename);
	if (access(filepath, F_OK) != 0) {
		ret_code = 1;
		goto OUT;
	}

	md5_hash_file(filepath, md5);
	if (memcmp(md5, filemd5, 32)) {
		ret_code = 2;
		goto OUT;
	}

	pnr_msglog_dbinsert(index, filetype, 0, MSG_STATUS_SENDOK, user_id, "", filename, userkey, 
		NULL, filepath + strlen(WS_SERVER_INDEX_FILEPATH), filesize);
		
OUT:
	ret_root = cJSON_CreateObject();
	if (!ret_root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
	
    ret_params = cJSON_CreateObject();
	if (!ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_params);
        return ERROR;
    }
		
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_UPLOADFILE));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
	
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad ret(%d)", *retmsg_len);
        free(ret_buff);
        return ERROR;
    }
	
    strcpy(retmsg, ret_buff);
    free(ret_buff);
	
	return OK;
}

/*****************************************************************************
 函 数 名  : im_delete_file_deal
 功能描述  : 删除文件
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月15日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_delete_file_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	char user_id[TOX_ID_STR_LEN + 1] = {0};
	char sql[1024] = {0};
	char filename[PNR_FILENAME_MAXLEN + 1] = {0};
	char filepath[PNR_FILEPATH_MAXLEN + 1] = {0};
	char *tmp_json_buff = NULL;
	char *ret_buff = NULL;
	char *errmsg = NULL;
    cJSON *tmp_item = NULL;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
	int index = 0;
	int ret_code = 0;
    char* p_filepath = NULL;
	
    if (!params) {
        return ERROR;
    }

    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "UserId", user_id, TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileName", filename, PNR_FILENAME_MAXLEN);
    index = get_indexbytoxid(user_id);
    if (index == 0) {
        ret_code = 3;
		goto OUT;
    }
    if(head->api_version >= PNR_API_VERSION_V5)
    {
        CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FilePath", sql, PNR_FILEPATH_MAXLEN);
        if(strcasecmp(sql,WS_SERVER_INDEX_FILEPATH) == OK)
        {
            strcpy(filepath,sql);
        }
        else
        {
            strcpy(filepath,WS_SERVER_INDEX_FILEPATH);
            strcat(filepath,sql);
        }
        p_filepath = filepath + strlen(WS_SERVER_INDEX_FILEPATH);
    }
    else
    {
        snprintf(filepath, sizeof(filepath), WS_SERVER_INDEX_FILEPATH"%s", filename);
    }
	if (access(filepath, F_OK) != 0) {
		ret_code = 1;
		goto OUT;
	}

	unlink(filepath);
    if(head->api_version >= PNR_API_VERSION_V5)
    {
        snprintf(sql, sizeof(sql), "delete from msg_tbl where ext='%s';", p_filepath);
    }
    else
    {
        snprintf(sql, sizeof(sql), "delete from msg_tbl where ext='%s';", filename);
    }
	if (sqlite3_exec(g_msglogdb_handle[index], sql, 0, 0, &errmsg)) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "sql(%s) err(%s)", sql, errmsg);
        sqlite3_free(errmsg);
		ret_code = 3;
    }

OUT:
	ret_root = cJSON_CreateObject();
	if (!ret_root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
	
    ret_params = cJSON_CreateObject();
	if (!ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_params);
        return ERROR;
    }
		
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_DELETEFILE));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
	
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad ret(%d)", *retmsg_len);
        free(ret_buff);
        return ERROR;
    }
	
    strcpy(retmsg, ret_buff);
    free(ret_buff);
	
	return OK;
}

/*****************************************************************************
 函 数 名  : im_get_disk_info_deal
 功能描述  : 获取磁盘信息
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月23日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_get_disk_info_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	int ret = OK;
	char buf[2048] = {0};
	cJSON *json_test = NULL;
	char *ret_buff = NULL;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
	int ret_code = 0;

	ret_root = cJSON_CreateObject();
	if (!ret_root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
	
    ret_params = cJSON_CreateObject();
	if (!ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_params);
        return ERROR;
    }

    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GETDISKDETAILINFO));

	ret = get_file_content("/tmp/disk.err", buf, sizeof(buf));
	if (ret) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get disk.err failed");
		ret_code = 1;
		goto OUT;
	}

	json_test = cJSON_Parse(buf);
	if (!json_test) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares disk.err(%s) err", buf);
		ret_code = 1;
		goto OUT;
	}

	ret_buff = cJSON_PrintUnformatted(json_test);
	cJSON_Delete(json_test);
	cJSON_AddItemToObject(ret_params, "mode", cJSON_CreateString(ret_buff));

	memset(buf, 0, sizeof(buf));
	ret = get_file_content("/tmp/disk.info", buf, sizeof(buf));
	if (ret) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get disk.info failed");
		ret_code = 1;
		goto OUT;
	}

	json_test = cJSON_Parse(buf);
	if (!json_test) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares disk.info(%s) err", buf);
		ret_code = 1;
		goto OUT;
	}

	ret_buff = cJSON_PrintUnformatted(json_test);
	cJSON_Delete(json_test);
	cJSON_AddItemToObject(ret_params, "info", cJSON_CreateString(ret_buff));

	memset(buf, 0, sizeof(buf));
	buf[0] = '{';
	ret = get_popen_content("/opt/bin/hdsmart.sh", &buf[1], sizeof(buf) - 2);
	if (ret) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get disk.size failed");
		ret_code = 1;
		goto OUT;
	}

	strcat(buf, "}");

	json_test = cJSON_Parse(buf);
	if (!json_test) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares disk.size(%s) err", buf);
		ret_code = 1;
		goto OUT;
	}

	ret_buff = cJSON_PrintUnformatted(json_test);
	cJSON_Delete(json_test);
	cJSON_AddItemToObject(ret_params, "size", cJSON_CreateString(ret_buff));

OUT:
	cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

	ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
	
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad ret(%d)", *retmsg_len);
        free(ret_buff);
        return ERROR;
    }
	
    strcpy(retmsg, ret_buff);
    free(ret_buff);
	
	return OK;	
}

/*****************************************************************************
 函 数 名  : im_get_disk_totalinfo_deal
 功能描述  : 获取磁盘统计信息
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int im_get_disk_totalinfo_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	char *ret_buff = NULL;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
    int json_index = 0;
	int i = 0;
	int ret_code = 0;
    struct disk_total_info totalinfo;
    struct dist_detail_info detailinfo[PNR_DISK_MAXNUM];
    
	ret_root = cJSON_CreateObject();
	if (!ret_root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
	
    ret_params = cJSON_CreateObject();
	if (!ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_params);
        return ERROR;
    }

    memset(&totalinfo,0,sizeof(totalinfo));
    memset(&detailinfo[0],0,PNR_DISK_MAXNUM*sizeof(struct dist_detail_info));
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GETDISKTOTALINFO));
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_get_disk_totalinfo_deal:g_pnrdevtype(%d)",g_pnrdevtype);
    if(g_pnrdevtype == PNR_DEV_TYPE_ONESPACE)
    {
    	int ret = OK;
    	char buf[2048] = {0};
        char value_cache[MANU_NAME_MAXLEN+1]= {0};
    	cJSON *json_cache = NULL;
        cJSON *subjson_cache= NULL;
        cJSON *subjson_item = NULL;
    	char *tmp_json_buff = NULL;
        cJSON *tmp_item = NULL;
        int slot = 0;
        //首先获取当前磁盘配置模式和数量
        ret = get_file_content("/tmp/disk.err", buf, sizeof(buf));
        if (ret) {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get disk.err failed");
            ret_code = 1;
            goto OUT;
        }
    
        json_cache = cJSON_Parse(buf);
        if (!json_cache) {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares disk.err(%s) err", buf);
            ret_code = 1;
            goto OUT;
        }
        CJSON_GET_VARINT_BYKEYWORD(json_cache, tmp_item, tmp_json_buff, "count", totalinfo.count, 0);
        if(totalinfo.count < 0 || totalinfo.count > PNR_DISK_MAXNUM)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares get count(%d) err", totalinfo.count);
            ret_code = 1;
            goto OUT;
        }
        CJSON_GET_VARSTR_BYKEYWORD(json_cache, tmp_item, tmp_json_buff, "mode", value_cache, MANU_NAME_MAXLEN);
        for(i=PNR_DISK_MODE_NONE;i<PNR_DISK_MODE_BUTT; i++)
        {
            if(strcasecmp(value_cache,g_valid_disk_mode[i]) == OK)
            {
                totalinfo.mode = i;
                break;
            }
        }
        if(i >= PNR_DISK_MODE_BUTT)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares get mode(%s) err", value_cache);
            ret_code = 1;
            goto OUT;
        }
        if(get_disk_capacity(totalinfo.count,totalinfo.used_capacity,totalinfo.total_capacity,&totalinfo.used_percent) != OK)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get_disk_capacity err");
            ret_code = 1;
            goto OUT;
        }
        //DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_get_disk_totalinfo_deal:get count(%d)",totalinfo.count);
        if(totalinfo.count > 0)
        {
            //获取实时温度信息
            memset(buf, 0, sizeof(buf));
        	buf[0] = '{';
        	ret = get_popen_content("/opt/bin/hdsmart.sh", &buf[1], sizeof(buf) - 2);
        	if (ret) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get disk.size failed");
        		ret_code = 1;
        		goto OUT;
        	}
        	strcat(buf, "}");
        	json_cache = cJSON_Parse(buf);
        	if (!json_cache) 
            {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart(%s) err", buf);
        		ret_code = 1;
        		goto OUT;
        	}
            subjson_cache = cJSON_GetObjectItem(json_cache, "hds");
        	if (!subjson_cache)
            {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart get subjson_cache failed");
        		ret_code = 1;
        		goto OUT;
        	}   
            subjson_item = cJSON_GetArrayItem(subjson_cache,0);
        	if (!subjson_item)
            {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart get tmp_item 0 failed");
        		ret_code = 1;
        		goto OUT;
        	}
            CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "slot", slot, 0);
            if(slot < 0 || slot >= PNR_DISK_MAXNUM)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart get slot (%d) failed",slot);
        		ret_code = 1;
        		goto OUT;
            }
            subjson_item = cJSON_GetObjectItem(subjson_item, "smart");
        	if (!subjson_item)
            {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart get tmp_item 0 failed");
        		ret_code = 1;
        		goto OUT;
        	}  
            CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Power_On_Hours", detailinfo[slot].power_on, 0);
            CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Temperature_Celsius", detailinfo[slot].temperature, 0);
            detailinfo[slot].status = PNR_DISK_STATUS_RUNNING;
            if(totalinfo.count == PNR_DISK_MAXNUM)
            {
                subjson_item = cJSON_GetArrayItem(subjson_cache,1);
            	if (!subjson_item)
                {
            		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart get tmp_item 0 failed");
            		ret_code = 1;
            		goto OUT;
            	}
                CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "slot", slot, 0);
                if(slot < 0 || slot >= PNR_DISK_MAXNUM)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart get slot (%d) failed",slot);
            		ret_code = 1;
            		goto OUT;
                }
                subjson_item = cJSON_GetObjectItem(subjson_item, "smart");
            	if (!subjson_item)
                {
            		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart get tmp_item 0 failed");
            		ret_code = 1;
            		goto OUT;
            	}  
                CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Power_On_Hours", detailinfo[slot].power_on, 0);
                CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Temperature_Celsius", detailinfo[slot].temperature, 0);
                detailinfo[slot].status = PNR_DISK_STATUS_RUNNING;
            }
            //获取磁盘信息
            memset(buf, 0, sizeof(buf));
        	ret = get_file_content("/tmp/disk.info", buf, sizeof(buf));
        	if (ret) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get disk.info failed");
        		ret_code = 1;
        		goto OUT;
        	}
        	json_cache= cJSON_Parse(buf);
        	if (!json_cache) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares disk.info(%s) err", buf);
        		ret_code = 1;
        		goto OUT;
        	}
            if(totalinfo.mode == PNR_DISK_MODE_BASIC)
            {
                json_index = 0;
            }
            else
            {
                json_index = 1;
            }
            subjson_cache = cJSON_GetArrayItem(json_cache,json_index);
        	if (!subjson_cache) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares subjson_cache 1 err", buf);
        		ret_code = 1;
        		goto OUT;
        	}
            subjson_item = cJSON_GetObjectItem(subjson_cache, "info");
        	if (!subjson_item) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares subjson_item 1 err", buf);
        		ret_code = 1;
        		goto OUT;
        	}
            CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "slot", slot, 0);
            if(slot < 0 || slot >= PNR_DISK_MAXNUM)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart disk.info slot (%d) failed",slot);
                ret_code = 1;
                goto OUT;
            }
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "User Capacity", detailinfo[slot].capacity, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Device Model", detailinfo[slot].device, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Serial Number", detailinfo[slot].serial, MANU_NAME_MAXLEN);
            if(totalinfo.count == PNR_DISK_MAXNUM)
            {
                subjson_cache = cJSON_GetArrayItem(json_cache,json_index+1);
            	if (!subjson_cache) {
            		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares subjson_cache 1 err", buf);
            		ret_code = 1;
            		goto OUT;
            	}
                subjson_item = cJSON_GetObjectItem(subjson_cache, "info");
                if (!subjson_item) {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares subjson_item 2 err", buf);
                    ret_code = 1;
                    goto OUT;
                }
                CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "slot", slot, 0);
                if(slot < 0 || slot >= PNR_DISK_MAXNUM)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares hdsmart disk.info slot (%d) failed",slot);
                    ret_code = 1;
                    goto OUT;
                }
                CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "User Capacity", detailinfo[slot].capacity, MANU_NAME_MAXLEN);
                CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Device Model", detailinfo[slot].device, MANU_NAME_MAXLEN);
                CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Serial Number", detailinfo[slot].serial, MANU_NAME_MAXLEN);
                //DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_get_disk_totalinfo_deal:get disk(%d)(Serial Number:%s)",slot,detailinfo[slot].serial);
            }
        }        
    }
    else
    {
#if 0
        ret_code = OK;
        totalinfo.count = PNR_DISK_MAXNUM;
        totalinfo.mode = PNR_DISK_MODE_RAID1;
        strcpy(totalinfo.used_capacity,"756M");
        strcpy(totalinfo.total_capacity,"1.7G");
        detailinfo[0].slot = 0;
        detailinfo[0].status = PNR_DISK_STATUS_RUNNING;
        detailinfo[0].power_on = 41;
        detailinfo[0].temperature = 37;
        strcpy(detailinfo[0].capacity,"1.5G");
        strcpy(detailinfo[0].device,"WDC WD10EZEX-08WN4A0");
        strcpy(detailinfo[0].serial,"WD-WCC6Y4UV92L8");
        detailinfo[1].slot = 1;
        detailinfo[1].status = PNR_DISK_STATUS_NOINIT;
#else
        ret_code = OK;
        totalinfo.count = 0;
        totalinfo.mode = PNR_DISK_MODE_NONE;
        if(get_disk_capacity(totalinfo.count,totalinfo.used_capacity,totalinfo.total_capacity,&totalinfo.used_percent) != OK)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get_disk_capacity err");
            ret_code = 1;
            goto OUT;
        }
#endif
    }
OUT:
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    if(ret_code == OK)
    {
        cJSON_AddItemToObject(ret_params, "Mode", cJSON_CreateNumber(totalinfo.mode));
        cJSON_AddItemToObject(ret_params, "Count", cJSON_CreateNumber(totalinfo.count));
        cJSON_AddItemToObject(ret_params, "UsedCapacity", cJSON_CreateString(totalinfo.used_capacity));
        cJSON_AddItemToObject(ret_params, "TotalCapacity", cJSON_CreateString(totalinfo.total_capacity));
        if(totalinfo.count > 0)
        {
            cJSON *pJsonArry = cJSON_CreateArray();
            if(pJsonArry == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
                cJSON_Delete(ret_params);
                return ERROR;
            }
            for(i = 0;i < PNR_DISK_MAXNUM;i++)
            {
                cJSON *pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    return ERROR;
                }
                cJSON_AddItemToArray(pJsonArry,pJsonsub); 
                cJSON_AddNumberToObject(pJsonsub,"Slot",i);
                cJSON_AddNumberToObject(pJsonsub,"Status",detailinfo[i].status);
                if(detailinfo[i].status == PNR_DISK_STATUS_RUNNING)
                {
                    cJSON_AddNumberToObject(pJsonsub,"PowerOn",detailinfo[i].power_on);
                    cJSON_AddNumberToObject(pJsonsub,"Temperature",detailinfo[i].temperature);
                    cJSON_AddStringToObject(pJsonsub,"Capacity",detailinfo[i].capacity);
                    cJSON_AddStringToObject(pJsonsub,"Device",detailinfo[i].device);
                    cJSON_AddStringToObject(pJsonsub,"Serial",detailinfo[i].serial);
                }
            }
            cJSON_AddItemToObject(ret_params,"Info", pJsonArry);
        }
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < VERSION_MAXLEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
	return OK;	
}
/*****************************************************************************
 函 数 名  : im_get_disk_detailinfo_deal
 功能描述  : 获取磁盘详细信息
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int im_get_disk_detailinfo_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	char *ret_buff = NULL;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
    cJSON *tmp_item = NULL;
	char *tmp_json_buff = NULL;
	int slot = -1;
	int ret_code = 0;
    struct dist_detail_info detailinfo;

    memset(&detailinfo,0,sizeof(struct dist_detail_info));
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Slot",slot,TOX_ID_STR_LEN);

    ret_root = cJSON_CreateObject();
	if (!ret_root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
	
    ret_params = cJSON_CreateObject();
	if (!ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_params);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GETDISKDETAILINFO));
    if(slot < 0 || slot >= PNR_DISK_MAXNUM)
    {
        ret_code = ERROR;
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"get slot(%d) error",slot);
    }
    else
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_get_disk_detailinfo_deal:g_pnrdevtype(%d)",g_pnrdevtype);
        if(g_pnrdevtype == PNR_DEV_TYPE_ONESPACE)
        {
        	int ret = OK;
        	char buf[2048] = {0};
        	cJSON *json_cache = NULL;
            cJSON *subjson_cache= NULL;
            cJSON *subjson_item = NULL;
        	char *tmp_json_buff = NULL;
            cJSON *tmp_item = NULL;
            int tmp_slot = 0;
            //获取磁盘信息
            memset(buf, 0, sizeof(buf));
        	ret = get_file_content("/tmp/disk.info", buf, sizeof(buf));
        	if (ret) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get disk.info failed");
        		ret_code = 1;
        		goto OUT;
        	}
        	json_cache= cJSON_Parse(buf);
        	if (!json_cache) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares disk.info(%s) err", buf);
        		ret_code = 1;
        		goto OUT;
        	}
            subjson_cache = cJSON_GetArrayItem(json_cache,1);
        	if (!json_cache) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares subjson_cache 1 err", buf);
        		ret_code = 1;
        		goto OUT;
        	}
            subjson_item = cJSON_GetObjectItem(subjson_cache, "info");
        	if (!subjson_item) {
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares subjson_cache 1 err", buf);
        		ret_code = 1;
        		goto OUT;
        	}
            CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "slot", tmp_slot, 0);
            if(slot != tmp_slot)
            {
                subjson_cache = cJSON_GetArrayItem(json_cache,2);
            	if (!json_cache) {
            		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares subjson_cache 1 err", buf);
            		ret_code = 1;
            		goto OUT;
            	}
                subjson_item = cJSON_GetObjectItem(subjson_cache, "info");
            	if (!subjson_item) {
            		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares subjson_cache 1 err", buf);
            		ret_code = 1;
            		goto OUT;
            	}
                CJSON_GET_VARINT_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "slot", tmp_slot, 0);
                if(slot != tmp_slot)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json pares get tmp_slot(%d) err", tmp_slot);
                    ret_code = 1;
                    goto OUT;
                }
            }
            ret_code = OK;
            detailinfo.slot = slot;
            detailinfo.status = PNR_DISK_STATUS_RUNNING;
            CJSON_GET_VARSTR_BYKEYWORD(subjson_cache, tmp_item, tmp_json_buff, "name", detailinfo.name, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Device Model", detailinfo.device, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Serial Number", detailinfo.serial, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Firmware Version", detailinfo.firmware, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Form Factor", detailinfo.formfactor, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "LU WWN Device Id", detailinfo.luwwndeviceid, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Model Family", detailinfo.modelfamily, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "User Capacity", detailinfo.capacity, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Sector Sizes", detailinfo.sectorsizes, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "Rotation Rate", detailinfo.rotationrate, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "ATA Version is", detailinfo.ataversion, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "SATA Version is", detailinfo.sataversion, MANU_NAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(subjson_item, tmp_item, tmp_json_buff, "SMART support is", detailinfo.smartsupport, MANU_NAME_MAXLEN);
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_get_disk_detailinfo_deal:get slot(%d) name(%s)Seriral(%s)",
                detailinfo.slot,detailinfo.name,detailinfo.serial);
        }
        else
        {
#if 0
            ret_code = OK;
            detailinfo.slot = slot;
            detailinfo.status = PNR_DISK_STATUS_RUNNING;
            strcpy(detailinfo.name,"/dev/sda");
            strcpy(detailinfo.modelfamily,"Western Digital Blue");
            strcpy(detailinfo.device,"WDC WD10EZEX-08WN4A0");
            strcpy(detailinfo.serial,"WD-WCC6Y4UV92L8");
            strcpy(detailinfo.firmware,"02.01A02");
            strcpy(detailinfo.formfactor,"3.5 inches");
            strcpy(detailinfo.luwwndeviceid,"5 0014ee 265bdf6ba");
            strcpy(detailinfo.capacity,"1,000,204,886,016 bytes [1.00 TB]");
            strcpy(detailinfo.sectorsizes,"512 bytes logical, 4096 bytes physical");
            strcpy(detailinfo.rotationrate,"7200 rpm");
            strcpy(detailinfo.ataversion,"ACS-3 T13/2161-D revision 3b");
            strcpy(detailinfo.sataversion,"SATA 3.1, 6.0 Gb/s");
            strcpy(detailinfo.smartsupport,"Available - device has SMART capability");
#else
            ret_code = OK;
            detailinfo.status  = PNR_DISK_STATUS_NONE;
#endif
        }
    }
OUT:
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    if(ret_code == OK)
    {
        cJSON_AddItemToObject(ret_params, "Slot", cJSON_CreateNumber(slot));
        cJSON_AddItemToObject(ret_params, "Status", cJSON_CreateNumber(detailinfo.status));
        if(detailinfo.status == PNR_DISK_STATUS_RUNNING)
        {
            cJSON_AddItemToObject(ret_params, "Name", cJSON_CreateString(detailinfo.name));
            cJSON_AddItemToObject(ret_params, "Device", cJSON_CreateString(detailinfo.device));
            cJSON_AddItemToObject(ret_params, "Serial", cJSON_CreateString(detailinfo.serial));
            cJSON_AddItemToObject(ret_params, "Firmware", cJSON_CreateString(detailinfo.firmware));
            cJSON_AddItemToObject(ret_params, "FormFactor", cJSON_CreateString(detailinfo.formfactor));
            cJSON_AddItemToObject(ret_params, "LUWWNDeviceId", cJSON_CreateString(detailinfo.luwwndeviceid));
            cJSON_AddItemToObject(ret_params, "ModelFamily", cJSON_CreateString(detailinfo.modelfamily));
            cJSON_AddItemToObject(ret_params, "Capacity", cJSON_CreateString(detailinfo.capacity));
            cJSON_AddItemToObject(ret_params, "SectorSizes", cJSON_CreateString(detailinfo.sectorsizes));
            cJSON_AddItemToObject(ret_params, "RotationRate", cJSON_CreateString(detailinfo.rotationrate));
            cJSON_AddItemToObject(ret_params, "ATAVersion", cJSON_CreateString(detailinfo.ataversion));
            cJSON_AddItemToObject(ret_params, "SATAVersion", cJSON_CreateString(detailinfo.sataversion));
            cJSON_AddItemToObject(ret_params, "SMARTsupport", cJSON_CreateString(detailinfo.smartsupport));
        }
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < VERSION_MAXLEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
	return OK;	
}
/*****************************************************************************
 函 数 名  : im_format_disk_deal
 功能描述  : 格式化磁盘
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月23日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_format_disk_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	char *tmp_json_buff = NULL;
	char *ret_buff = NULL;
    cJSON *tmp_item = NULL;
	cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
	cJSON *diskerr_j = NULL;
	int i = 0, ret = 0;
	int ret_code = 1;
	char mode[16] = {0};
	char diskerr[128] = {0};
	char cmd[512] = {0};

    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "Mode", mode, 16);
    i = 1;
	while (g_valid_disk_mode[i]) {
		if (!strcmp(mode, g_valid_disk_mode[i])) {
			ret_code = 0;
			break;
		}
		i++;
	}

	ret = get_file_content("/tmp/disk.err", diskerr, sizeof(diskerr));
	if (ret) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get disk.err failed");
		ret_code = 1;
		goto OUT;
	}

	diskerr_j = cJSON_Parse(diskerr);
	if (!diskerr_j) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "parse disk.err(%s) failed", diskerr);
		ret_code = 1;
		goto OUT;
	}

	snprintf(cmd, sizeof(cmd), "/opt/bin/formathd.sh %s %s %d %d %s ", 
		mode, 
		cJSON_GetObjectItem(diskerr_j, "mode")->valuestring,
		cJSON_GetObjectItem(diskerr_j, "count")->valueint,
		cJSON_GetObjectItem(diskerr_j, "errno")->valueint,
		cJSON_GetObjectItem(diskerr_j, "dev1")->valuestring);

	if (cJSON_GetObjectItem(diskerr_j, "dev2")) {
		strcat(cmd, cJSON_GetObjectItem(diskerr_j, "dev2")->valuestring);
	}

	//close db files before umount /sata
	for (i = 0; i <= PNR_IMUSER_MAXNUM; i++) {
		if (g_msglogdb_handle[i])
			sqlite3_close(g_msglogdb_handle[i]);

		if (g_msgcachedb_handle[i])
			sqlite3_close(g_msgcachedb_handle[i]);
	}

	pthread_mutex_lock(&g_formating_lock);
	g_formating = 1;
	pthread_mutex_unlock(&g_formating_lock);
	
	system(cmd);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr format cmd(%s)",cmd);
	g_format_reboot_time = time(NULL) + 5;
	
OUT:
	ret_root = cJSON_CreateObject();
	if (!ret_root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
	
    ret_params = cJSON_CreateObject();
	if (!ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "json create err");
        cJSON_Delete(ret_params);
        return ERROR;
    }
		
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_FORMATDISK));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
	
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad ret(%d)", *retmsg_len);
        free(ret_buff);
        return ERROR;
    }
	
    strcpy(retmsg, ret_buff);
    free(ret_buff);
	
	return OK;
}

/*****************************************************************************
 函 数 名  : im_reboot_deal
 功能描述  : 设备重启
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月23日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_reboot_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char fromid[TOX_ID_STR_LEN+1] = {0};

    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"User",fromid,TOX_ID_STR_LEN);

    //参数检查
    if(strcmp(fromid,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].toxid) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad fromid(%s) need(%s)",fromid,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].toxid);
        ret_code = PNR_REBOOT_RETURN_NOOWNER;
    }
    else
    {
        //这时候也不允许format操作
        pthread_mutex_lock(&g_formating_lock);
    	g_formating = 1;
    	pthread_mutex_unlock(&g_formating_lock);
    	g_format_reboot_time = time(NULL) + 5;
        DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"###node will reboot###");
    }
    
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_REBOOT));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(fromid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_replaymsg_deal
  Description: IM模块replay消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_replaymsg_deal(cJSON *params, int cmd, struct imcmd_msghead_struct *head, 
    int friendnum)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret = 0;
    char toid[TOX_ID_STR_LEN+1] = {0};
    int index = 0;

    if(params == NULL)
    {
        return ERROR;
    }

    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff, "ToId", toid, TOX_ID_STR_LEN);
    index = get_indexbytoxid(toid);
    if (index == 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get toid(%s) index err", toid);
        return ERROR;
    }

	pnr_msgcache_dbdelete(head->msgid, index);
    switch(cmd)
    {
        //case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
        case PNR_IM_CMDTYPE_PUSHMSG:
        case PNR_IM_CMDTYPE_PUSHFILE:
        case PNR_IM_CMDTYPE_PUSHFILE_TOX:
        case PNR_IM_CMDTYPE_GROUPMSGPUSH:
            CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Retcode",ret,0);
#if 0//现在不在这里作消息推送
            if(g_imusr_array.usrnode[index].appactive_flag == PNR_APPACTIVE_STATUS_BACKEND)
            {
                post_newmsg_notice(g_daemon_tox.user_toxid,toid,PNR_POSTMSG_PAYLOAD,TRUE); 
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"###user(%d:%s)  post_newmsg_notice###",index,toid);
            }
#endif
            break;  

        case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
        case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
        case PNR_IM_CMDTYPE_DELMSGPUSH:
        case PNR_IM_CMDTYPE_DELFRIENDPUSH:
        case PNR_IM_CMDTYPE_READMSGPUSH:
        case PNR_IM_CMDTYPE_USERINFOPUSH:
        case PNR_IM_CMDTYPE_GROUPINVITEPUSH:
        case PNR_IM_CMDTYPE_VERIFYGROUPPUSH:
        case PNR_IM_CMDTYPE_GROUPSYSPUSH:
        //case PNR_IM_CMDTYPE_GROUPMSGPUSH:
            CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Retcode",ret,0);
            break;  
        default:
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad cmd(%d) failed",cmd);
            return ERROR;
    }
	
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"rec msg(%d) ret(%d)",cmd,ret);
    return OK;
}

/**********************************************************************************
  Function:      im_msghead_parses
  Description: IM模块消息头部解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_msghead_parses(cJSON * root,cJSON * params,struct imcmd_msghead_struct* phead)
{
    char action_buff[PNR_IMCMD_PARAMS_KEYWORD_MAXLEN+1]={0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    if(root == NULL || phead == NULL)
    {
        return ERROR;
    }

    CJSON_GET_VARLONG_BYKEYWORD(root,tmp_item,tmp_json_buff,"timestamp",phead->timestamp,0);
    CJSON_GET_VARSTR_BYKEYWORD(root,tmp_item,tmp_json_buff,"appid",phead->appid,APPID_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(root,tmp_item,tmp_json_buff,"apiversion",phead->api_version,0);

    //路由发送过来的消息不解析msgid
    if (!phead->no_parse_msgid) {
        CJSON_GET_VARLONG_BYKEYWORD(root,tmp_item,tmp_json_buff,"msgid",phead->msgid,0);
    }
    if (phead->iftox) {
		CJSON_GET_VARINT_BYKEYWORD(root,tmp_item,tmp_json_buff,"offset",phead->offset,0);	
        if(phead->offset)
        {
            return OK;
        }
    }

    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Action",action_buff,PNR_IMCMD_PARAMS_KEYWORD_MAXLEN);

	switch(action_buff[0])
    {
        case 'a':
        case 'A':
            if(strcasecmp(action_buff,PNR_IMCMD_ADDFRIEDNREQ) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_ADDFRIENDREQ;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_ADDFRIENDREPLY) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_ADDFRIENDREPLY;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_ADDFRIENDPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_ADDFRIENDPUSH;
            } 
            else if(strcasecmp(action_buff,PNR_IMCMD_ADDREIENDDEAL) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_ADDFRIENDDEAL;
            }  
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
        case 'c':
        case 'C':
            if(strcasecmp(action_buff,PNR_IMCMD_CREATENORMALUSER) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_CREATENORMALUSER;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_CHANGEREMARKS) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_CHANGEREMARKS;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_CREATEGROUP) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_CREATEGROUP;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
        case 'd':
        case 'D':
            if(strcasecmp(action_buff,PNR_IMCMD_DESTORY) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_DESTORY;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_DELFRIENDCMD) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_DELFRIENDCMD;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_DELFRIENDPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_DELFRIENDPUSH;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_DELMSG) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_DELMSG;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_DELETEFILE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_DELETEFILE;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_DELUSER) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_DELUSER;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
		case 'f':
		case 'F':
			if(strcasecmp(action_buff,PNR_IMCMD_FORMATDISK) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_FORMATDISK;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_FILERENAME) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_FILERENAME;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_FILEFORWARD) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_FILEFORWARD;
            }
			else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
			break;
		case 'g':
		case 'G':
			if(strcasecmp(action_buff,PNR_IMCMD_GETDISKDETAILINFO) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GETDISKDETAILINFO;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GETDISKTOTALINFO) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GETDISKTOTALINFO;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPINVITEPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPINVITEPUSH;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPINVITEDEAL) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPINVITEDEAL;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPQUIT) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPQUIT;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPLISTPULL) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPLISTPULL;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPUSERPULL) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPUSERPULL;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPMSGPULL) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPMSGPULL;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPSENDMSG) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPSENDMSG;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPSENDFILEPRE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPSENDFILEPRE;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPSENDFILEDONE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPSENDFILEDONE;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPDELMSG) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPDELMSG;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPMSGPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPMSGPUSH;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPCONFIG) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPCONFIG;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_GROUPSYSPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_GROUPSYSPUSH;
            }
            else if (strcasecmp(action_buff, PNR_IMCMD_VERIFYGROUP) == OK)
			{
				phead->im_cmdtype = PNR_IM_CMDTYPE_VERIFYGROUP;
			}
            else if(strcasecmp(action_buff, PNR_IMCMD_VERIFYGROUPPUSH) == OK)
            {
				phead->im_cmdtype = PNR_IM_CMDTYPE_VERIFYGROUPPUSH;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
			break;
        case 'h':
        case 'H':
            if(strcasecmp(action_buff,PNR_IMCMD_HEARTBEAT) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_HEARTBEAT;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
        case 'i':
        case 'I':
            if(strcasecmp(action_buff,PNR_IMCMD_INVITEGROUP) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_INVITEGROUP;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
        case 'l':
        case 'L':
            if(strcasecmp(action_buff,PNR_IMCMD_LOGINIDENTIFY) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_LOGINIDENTIFY;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_LOGIN) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_LOGIN;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_LOGOUT) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_LOGOUT;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
        case 'o':
        case 'O':
            if(strcasecmp(action_buff,PNR_IMCMD_ONLINESTATUSCHECK) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_ONLINESTATUSCHECK;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_ONLINESTATUSPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_ONLINESTATUSPUSH;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
        case 'p':
        case 'P':
            if(strcasecmp(action_buff,PNR_IMCMD_PUSHMSG) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PUSHMSG;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_DELMSGPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_DELMSGPUSH;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_PULLMSG) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PULLMSG;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_PULLFRIEDN) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PULLFRIEND;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_PUSHFILE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PUSHFILE;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_PULLUSERLIST) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PULLUSERLIST;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_PULLFILE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PULLFILE;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_PREREGISTER) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PREREGISTER;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_PULLUSERLIST) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PULLUSERLIST;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_PUSHFILE_TOX) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PUSHFILE_TOX;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_PULLFILELIST) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PULLFILELIST;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_PULLTMPACCOUNT) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_PULLTMPACCOUNT;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
		case 'q':
		case 'Q':
			if (strcasecmp(action_buff, PNR_IMCMD_QUERYFRIEND) == OK)
			{
				phead->im_cmdtype = PNR_IM_CMDTYPE_GET_RELATIONSHIP;
			}
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }  
			break;
        case 'r':
        case 'R':
            if(strcasecmp(action_buff,PNR_IMCMD_REGISTER) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_REGISTER;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_RECOVERYIDENTIFY) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_RECOVERYIDENTIFY;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_RECOVERY) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_RECOVERY;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_READMSGPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_READMSGPUSH;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_READMSG) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_READMSG;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_ROUTERLOGIN) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_ROUTERLOGIN;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_RESETROUTERKEY) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_RESETROUTERKEY;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_RESETROUTERNAME) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_RESETROUTERNAME;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_RESETUSERIDCODE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_RESETUSERIDCODE;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_REBOOT) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_REBOOT;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }        
            break;
        case 's':
        case 'S':
            if(strcasecmp(action_buff,PNR_IMCMD_SENDMSG) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_SENDMSG;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_SENDFILE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_SENDFILE;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_SYNCHDATAFILE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_SYNCHDATAFILE;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_SYSDEBUGCMD) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_SYSDEBUGMSG;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_SYSDERLYCMD) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_SYSDERLYMSG;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break; 
        case 'u':
        case 'U':
            if(strcasecmp(action_buff,PNR_IMCMD_USERINFOPUSH) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_USERINFOPUSH;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_USERINFOUPDATE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_USERINFOUPDATE;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_UPLOADFILEREQ) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_UPLOADFILEREQ;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_UPLOADFILE) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_UPLOADFILE;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_UPLOADAVATAR) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_UPLOADAVATAR;
            }
			else if(strcasecmp(action_buff,PNR_IMCMD_UPDATEAVATAR) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_UPDATEAVATAR;
            }
            else if(strcasecmp(action_buff,PNR_IMCMD_USRDEBUGCMD) == OK)
            {
                phead->im_cmdtype = PNR_IM_CMDTYPE_USRDEBUGMSG;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }
            break;
 		case 'v':
		case 'V':
			if (strcasecmp(action_buff, PNR_IMCMD_VERIFYGROUP) == OK)
			{
				phead->im_cmdtype = PNR_IM_CMDTYPE_VERIFYGROUP;
			}
            else if(strcasecmp(action_buff, PNR_IMCMD_VERIFYGROUPPUSH) == OK)
            {
				phead->im_cmdtype = PNR_IM_CMDTYPE_VERIFYGROUPPUSH;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            }  
			break;       
        default:
            phead->im_cmdtype = PNR_IM_CMDTYPE_BUTT;
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad action(%s)",action_buff);
            break;
    }
    return OK;
}

/*****************************************************************************
 函 数 名  : im_sendfile_get_free_node
 功能描述  : 获取未使用的sendfile结构体索引
 输入参数  : 无
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年9月28日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_sendfile_get_free_node(int userindex)
{
	int i = 0;

	for (i = 0; i < PNR_MAX_SENDFILE_NUM; i++) {
		if (g_imusr_array.usrnode[userindex].file[i].status == FILE_UPLOAD_INIT)
			return i;
	}

	return -1;
}

/*****************************************************************************
 函 数 名  : im_sendfile_get_node_byfd
 功能描述  : 根据文件fd获取sendfile结构体index
 输入参数  : int sock  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年9月28日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_sendfile_get_node_byfd(int fd, int userindex)
{
	int i = 0;
	
	for (i = 0; i < PNR_MAX_SENDFILE_NUM; i++) {
		if (g_imusr_array.usrnode[userindex].file[i].fd == fd)
			return i;
	}

	return -1;
}

/*****************************************************************************
 函 数 名  : im_sendfile_cmd_deal
 功能描述  : 发送文件消息处理
 输入参数  : cJSON * params   
             char* retmsg     
             int* retmsg_len  
             int* plws_index  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年9月27日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_sendfile_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
	struct im_sendfile_struct *msg;
    char *tmp_json_buff = NULL;
    cJSON *tmp_item = NULL;
    int ret_code = 0;
    char *ret_buff = NULL;
    int index = 0;
    char md5[PNR_MD5_VALUE_MAXLEN+1] = {0};
    char fileinfo[PNR_FILEINFO_MAXLEN+1] = {0};
    cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;

	if (!params)
        return ERROR;

    msg = (struct im_sendfile_struct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    head->forward = TRUE;

	//解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FromId", msg->fromuser_toxid, TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "ToId", msg->touser_toxid, TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileName", msg->filename, UPLOAD_FILENAME_MAXLEN);
	CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileMD5", msg->md5, PNR_MD5_VALUE_MAXLEN);
	CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileInfo", fileinfo, PNR_FILEINFO_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileId", msg->fd, 0);
	CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileSize", msg->filesize, 0);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileType", msg->filetype, 0);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "SrcKey", msg->srckey, PNR_RSA_KEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "DstKey", msg->dstkey, PNR_RSA_KEY_MAXLEN);
	msg->msgtype = msg->filetype;

    index = get_indexbytoxid(msg->fromuser_toxid);
    if (index == 0) {
        ret_code = PNR_FILESEND_RETCODE_FAILED;
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "get fromuser_toxid(%s) failed", msg->fromuser_toxid);
        goto SENDRET;
    } else {
        if (*plws_index == 0)
            *plws_index = index;
    }
                
    snprintf(msg->fullfilename, UPLOAD_FILENAME_MAXLEN, "%ss/%s", 
        g_imusr_array.usrnode[index].userdata_pathurl, msg->filename);

    if (access(msg->fullfilename, F_OK) != 0) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "can not find file(%s)", msg->fullfilename);
        ret_code = PNR_FILESEND_RETCODE_NOFILE;
	} else {
        md5_hash_file(msg->fullfilename, md5);
        
    	if (strlen(msg->fromuser_toxid) != TOX_ID_STR_LEN) {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad userid(%s)", msg->fromuser_toxid);
            ret_code = PNR_FILESEND_RETCODE_FAILED;
        } else if (strcmp(msg->fromuser_toxid, msg->touser_toxid) == OK) {
           DEBUG_PRINT(DEBUG_LEVEL_ERROR, "userid repeat(%s->%s)",
                msg->fromuser_toxid, msg->touser_toxid); 
           ret_code = PNR_FILESEND_RETCODE_FAILED;
        } else if (strcmp(md5, msg->md5)) {
           DEBUG_PRINT(DEBUG_LEVEL_ERROR, "check md5 err"); 
           ret_code = PNR_FILESEND_RETCODE_MD5;
        } else {
            if(strlen(fileinfo) > 0)
            {
                strcat(msg->fullfilename,",");
                strcat(msg->fullfilename,fileinfo);
            }
            pnr_msglog_getid(index, &msg->log_id);
            pnr_msglog_dbupdate(index, msg->msgtype, msg->log_id,MSG_STATUS_SENDOK,msg->fromuser_toxid,
				msg->touser_toxid, msg->filename, msg->srckey, msg->dstkey, msg->fullfilename, msg->filetype);

            if (head->iftox) {
                head->toxmsg = msg;
                head->im_cmdtype = PNR_IM_CMDTYPE_PUSHFILE_TOX;
                head->to_userid = get_indexbytoxid(msg->touser_toxid);
            }
            
			ret_code = PNR_FILESEND_RETCODE_OK;
        }
    }

SENDRET:
    
	ret_root = cJSON_CreateObject();
    ret_params = cJSON_CreateObject();
    if (ret_root == NULL || ret_params == NULL) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
	
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_SENDFILE));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(msg->fromuser_toxid));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(msg->touser_toxid));
    cJSON_AddItemToObject(ret_params, "FileId", cJSON_CreateNumber(msg->fd));
    cJSON_AddItemToObject(ret_params, "FileType", cJSON_CreateNumber(msg->filetype));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msg->log_id));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
	
    strcpy(retmsg, ret_buff);
    free(ret_buff);

    if (!head->iftox)
        free(msg);
    
    return OK;

ERR:
    if (!head->iftox)
        free(msg);
    
    return ERROR;
}

/*****************************************************************************
 函 数 名  : im_rcv_file_deal
 功能描述  : 接收文件
 输入参数  : char *pmsg      
             int msg_len     
             int plws_index  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年9月27日
    作    者   : lichao
    修改内容   : 新生成函数
已经没用到了
*****************************************************************************/
int im_rcv_file_deal(char *pmsg, int msg_len, char *retmsg, int *retmsg_len, 
	int *ret_flag, int plws_index, int fileindex)
{
	int ret_code;
	char *ret_buff = NULL;
	struct im_sendfile_struct *file = &(g_imusr_array.usrnode[plws_index].file[fileindex]);
	char md5[33] = {0};
	int index = 0;
	
	*ret_flag = FALSE;
	
	file->status = FILE_UPLOAD_RUNNING;
	file->rcvlen += msg_len;

	DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "rcvdata[%d-%d-%d]", msg_len, plws_index, fileindex);
	DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "param[%d-%d-%d-%d-%s-%s-%s]", 
		plws_index, fileindex, file->filesize, file->fd, file->fullfilename, file->filename, file->md5);
	DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "rcv len:%d", file->rcvlen);
	
	write(file->fd, pmsg, msg_len);

	if (file->rcvlen >= file->filesize) {
		*ret_flag = TRUE;

		DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "rcv file(%s) complete", file->fullfilename);
		//printf("rcv file(%s) complete\n", file->fullfilename);
		
		//构建响应消息
		cJSON *ret_root = cJSON_CreateObject();
	    cJSON *ret_params = cJSON_CreateObject();
	    if (ret_root == NULL || ret_params == NULL) {
	        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "create json err");

			if (ret_root)
				cJSON_Delete(ret_root);

			if (ret_params)
				cJSON_Delete(ret_params);
			
	        goto ERR;
	    }

		ret_code = PNR_MSGSEND_RETCODE_OK;

		md5_hash_file(file->fullfilename, md5);
		DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "md5[%s]", md5);

		if (strncasecmp(md5, file->md5, 32)) {
			DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "md5[%s-%s]", file->md5, md5);
			ret_code = 2;
		}
			
	    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
	    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
	    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));

	    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_SENDFILE_END));
	    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
	    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(file->log_id));
	    cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(file->fromuser_toxid));
	    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(file->touser_toxid));
	    cJSON_AddItemToObject(ret_root, "params", ret_params);

	    ret_buff = cJSON_PrintUnformatted(ret_root);
	    cJSON_Delete(ret_root);
	    
	    *retmsg_len = strlen(ret_buff);
	    if (*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN) {
	        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
	        free(ret_buff);
	        goto ERR;
	    }
		
	    strcpy(retmsg, ret_buff);
	    free(ret_buff);

		//tox发送到对端
		if (ret_code == PNR_MSGSEND_RETCODE_OK) {
			index = get_indexbytoxid(file->touser_toxid);
	        //对于目标好友为本地用户
	        if (index != 0)
	            im_pushmsg_callback(index,PNR_IM_CMDTYPE_PUSHFILE,TRUE,PNR_API_VERSION_V5,(void *)file);
	        else
	            im_pushmsg_callback(index,PNR_IM_CMDTYPE_PUSHFILE,FALSE,PNR_API_VERSION_V5,(void *)file);
		}

		memset(file, 0, sizeof(struct im_sendfile_struct));
	}

	return OK;

ERR:
	memset(file, 0, sizeof(struct im_sendfile_struct));
	return ERROR;
}

/*****************************************************************************
 函 数 名  : im_pullfile_cmd_deal
 功能描述  : 拉取文件
 输入参数  : cJSON * params      
             int msg_len     
             int plws_index  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年12月03日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int im_pullfile_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_sendfile_struct file_info;
    char *tmp_json_buff = NULL;
    cJSON *tmp_item = NULL;
    int ret_code = 0;
    char *ret_buff = NULL;
    int index = 0;
    cJSON *ret_root = NULL;
    cJSON *ret_params = NULL;
	int fileowner = 0;
    int filefrom = 0;
    int gid = 0;
    char filepath[PNR_FILEPATH_MAXLEN+1] = {0};

    if (!params)
        return ERROR;

    memset(&file_info,0,sizeof(file_info));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FromId", file_info.fromuser_toxid, TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "ToId", file_info.touser_toxid, TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileName", file_info.filename, UPLOAD_FILENAME_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "MsgId", file_info.log_id, 0);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileOwner", fileowner, 0);
    CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FileFrom", filefrom, 0);
    if(head->api_version >= PNR_API_VERSION_V5)
    {
        CJSON_GET_VARINT_BYKEYWORD(params, tmp_item, tmp_json_buff, "FilePath", filefrom, 0);
    }

    index = get_indexbytoxid(file_info.touser_toxid);
    if (index == 0) 
    {
        ret_code = PNR_FILESEND_RETCODE_FAILED;
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "get touser_toxid(%s) failed", file_info.touser_toxid);
        return ERROR;
    } 
    else {
        if (*plws_index == 0)
            *plws_index = index;
    }
   
    if (fileowner == PNR_FILE_GROUP) {
        gid=get_gidbygrouphid(file_info.fromuser_toxid);
        if(gid < 0)
        {
            ret_code = PNR_FILESEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO, "get gid(%s) failed", file_info.fromuser_toxid);
            return ERROR;
        }
        snprintf(file_info.fullfilename, UPLOAD_FILENAME_MAXLEN, "%s%sg%d/%s", 
           DAEMON_PNR_USERDATA_DIR,PNR_GROUP_DATA_PATH,gid, file_info.filename);
    } 
    else if (fileowner == PNR_FILE_OWNER_SELF) {
    	snprintf(file_info.fullfilename, UPLOAD_FILENAME_MAXLEN, "%ss/%s", 
        	g_imusr_array.usrnode[index].userdata_pathurl, file_info.filename);
	} else if (fileowner == PNR_FILE_OWNER_FRIEND) {
		snprintf(file_info.fullfilename, UPLOAD_FILENAME_MAXLEN, "%sr/%s", 
        	g_imusr_array.usrnode[index].userdata_pathurl, file_info.filename);
	}else if (fileowner == PNR_FILE_OWNER_UPLOAD) {
		snprintf(file_info.fullfilename, UPLOAD_FILENAME_MAXLEN, "%su/%s", 
        	g_imusr_array.usrnode[index].userdata_pathurl, file_info.filename);
	}
    else if(fileowner == PNR_FILE_AVATAR)
    {
        if(strstr(file_info.filename,PNR_AVATAR_DIR) == NULL)
        {
    		snprintf(file_info.fullfilename, UPLOAD_FILENAME_MAXLEN, "%s%s%s", 
        	    DAEMON_PNR_USERDATA_DIR,PNR_AVATAR_DIR, file_info.filename);
        }
        else
        {
    		snprintf(file_info.fullfilename, UPLOAD_FILENAME_MAXLEN, "%s%s", 
        	    DAEMON_PNR_USERDATA_DIR, file_info.filename);
        }
    }
    else
    {
        ret_code = PNR_FILESEND_RETCODE_FAILED;
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "get fileowner(%d) err", fileowner);
        goto err;
    }
    //在V5版本之后文件在节点上保存的文件名优化了
    if(head->api_version >= PNR_API_VERSION_V5)
    {
        memset(file_info.fullfilename,0,PNR_FILEPATH_MAXLEN);
        strcpy(file_info.fullfilename,WS_SERVER_INDEX_FILEPATH);
        strcat(file_info.fullfilename,filepath);
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"new rcvfile bin reset filename(%s)",file_info.fullfilename);
    }
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_pullfile_cmd_deal:get fileowner(%d) fullfilename(%s)",fileowner,file_info.fullfilename);
    if (access(file_info.fullfilename, F_OK) != 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "can not find file(%s)", file_info.fullfilename);
        ret_code = PNR_FILESEND_RETCODE_NOFILE;
    }
    else if(g_imusr_array.usrnode[index].user_online_type != USER_ONLINE_TYPE_TOX)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "user(%d) was not connect by tox(%d)", g_imusr_array.usrnode[index].user_online_type);
        ret_code = PNR_FILESEND_RETCODE_NOFILE;
    }
    else 
	{
        md5_hash_file(file_info.fullfilename, file_info.md5);        
        if (strlen(file_info.fromuser_toxid) != TOX_ID_STR_LEN || 
            strlen(file_info.touser_toxid) != TOX_ID_STR_LEN) {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad userid(%s->%s)",
                file_info.fromuser_toxid, file_info.touser_toxid);
            ret_code = PNR_FILESEND_RETCODE_FAILED;
        } 
        //新版本的下载自己上传的文件，会出现to和form相同的情况
        /*else if (strcmp(file_info.fromuser_toxid, file_info.touser_toxid) == OK) {
           DEBUG_PRINT(DEBUG_LEVEL_ERROR, "userid repeat(%s->%s)",
                file_info.fromuser_toxid, file_info.touser_toxid); 
           ret_code = PNR_FILESEND_RETCODE_FAILED;
           } */ 
        else {
            file_info.filesize = get_file_size(file_info.fullfilename);
            ret_code = PNR_FILESEND_RETCODE_OK;
        }
    }
err:	
    ret_root = cJSON_CreateObject();
    ret_params = cJSON_CreateObject();
    if (ret_root == NULL || ret_params == NULL) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLFILE));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(file_info.log_id));
    cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(file_info.fromuser_toxid));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(file_info.touser_toxid));
    if(ret_code == PNR_FILESEND_RETCODE_OK)
    {
        cJSON_AddItemToObject(ret_params, "FileMD5", cJSON_CreateString(file_info.md5));
        cJSON_AddItemToObject(ret_params, "FileSize", cJSON_CreateNumber(file_info.filesize));
		cJSON_AddItemToObject(ret_params, "FileName", cJSON_CreateString(file_info.filename));
		cJSON_AddItemToObject(ret_params, "FileFrom", cJSON_CreateNumber(filefrom));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }

	//发送文件
    if (ret_code == PNR_FILESEND_RETCODE_OK) {
		DEBUG_PRINT(DEBUG_LEVEL_INFO, "send file(%s) to app(%d)", file_info.fullfilename, head->friendnum);
    	imtox_send_file_to_app(qlinkNode, head->friendnum, file_info.fromuser_toxid, file_info.fullfilename,file_info.log_id,filefrom);
    }
    
    strcpy(retmsg, ret_buff);
    free(ret_buff);
    return OK;
}



/**********************************************************************************
  Function:      im_create_normaluser_cmd_deal
  Description: IM模块创建普通用户命令解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_create_normaluser_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct pnr_account_struct account;
    struct pnr_account_struct admin_account;
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    int ret_len = 0;
    char* ret_buff = NULL;
    char qrcode_buf[PNR_QRCODE_MAXLEN+1] = {0};
    if(params == NULL)
    {
        return ERROR;
    }

    memset(&account,0,sizeof(account));
    memset(&admin_account,0,sizeof(admin_account));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"AdminUserId",admin_account.toxid,TOX_ID_STR_LEN);    
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Mnemonic",account.mnemonic,PNR_USERNAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"IdentifyCode",account.identifycode,PNR_IDCODE_MAXLEN);

    //rid和uid 检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_CREATE_NORMALUSER_RETCODE_BADRID;
    }
    else if(g_imusr_array.cur_user_num >= g_imusr_array.max_user_num)
    {
        ret_code = PNR_CREATE_NORMALUSER_RETCODE_NOMORE_USERS;
    }
    else
    {
        pnr_account_dbget_byuserid(&admin_account);
        if(admin_account.active != TRUE || admin_account.type != PNR_USER_TYPE_ADMIN)
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_create_normaluser_cmd_deal:admin_user(%s) active(%d) failed",admin_account.toxid,admin_account.active);
            ret_code = PNR_CREATE_NORMALUSER_RETCODE_BADUID;
        }
        else
        {
            //创建一个新的未激活账户
            g_account_array.normal_user_num++;
            g_account_array.total_user_num++;
            if(g_account_array.total_user_num >= g_imusr_array.max_user_num )
            {
                ret_code = PNR_CREATE_NORMALUSER_RETCODE_NOMORE_USERS;
            }
            else
            {
                pnr_create_usersn(PNR_USER_TYPE_NORMAL,g_account_array.normal_user_num,account.user_sn);
                account.active = FALSE;
                account.type = PNR_USER_TYPE_NORMAL;
                pnr_account_dbinsert(&account);
                memcpy(&g_account_array.account[g_account_array.total_user_num],&account,sizeof(account));
                ret_code = PNR_CREATE_NORMALUSER_RETCODE_OK;            
            }
        }
    }
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_CREATENORMALUSER));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "Routerid", cJSON_CreateString(router_id));
    if(ret_code == PNR_CREATE_NORMALUSER_RETCODE_OK)
    {
        cJSON_AddItemToObject(ret_params, "UserSN", cJSON_CreateString(account.user_sn));
        memset(qrcode_buf,0,sizeof(qrcode_buf));
        pnr_create_account_qrcode(account.user_sn,qrcode_buf,&ret_len);
        cJSON_AddStringToObject(ret_params,"Qrcode",qrcode_buf);
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "UserSN", cJSON_CreateString(""));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_userlogin_v2_deal
  Description: IM模块用户登陆命令解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_userlogin_v2_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head, int cur_friendnum,
	struct per_session_data__minimal *cur_pss)
{
    struct pnr_account_struct account;
    struct pnr_account_struct src_account;
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int need_asysn = 0;
    int data_version = 0;
    int index =0;
    int run =0;
    if(params == NULL)
    {
        return ERROR;
    }

    memset(&account,0,sizeof(account));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,PNR_USN_MAXLEN);    
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",account.toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"LoginKey",account.loginkey,PNR_LOGINKEY_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"DataFileVersion",data_version,PNR_IDCODE_MAXLEN);
    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_LOGIN_RETCODE_BAD_RID;
    }
    else if(strlen(account.toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v2_deal:bad uid(%d:%s)",strlen(account.toxid),account.toxid);
        ret_code = PNR_LOGIN_RETCODE_BAD_UID;
    }
    else
    {
        //根据toxid获取当前数据库中的账号信息
        memset(&src_account,0,sizeof(src_account));
        strcpy(src_account.toxid,account.toxid);
        pnr_account_dbget_byuserid(&src_account);
        if(src_account.type < PNR_USER_TYPE_ADMIN || src_account.type >= PNR_USER_TYPE_BUTT)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad account type(%d)",src_account.type);
            ret_code = PNR_LOGIN_RETCODE_OTHERS;
        }
        else if(src_account.active == FALSE)
        {
            ret_code = PNR_LOGIN_RETCODE_NEED_IDENTIFY;
        }
        //比较sn
        else if(strncasecmp(account.user_sn,src_account.user_sn,PNR_USN_MAXLEN) != OK)
        {
            ret_code = PNR_LOGIN_RETCODE_BAD_UID;
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v2_deal:input usn(%s) real usn(%s)",account.user_sn,src_account.user_sn);
        }
        else if(strncasecmp(account.loginkey,src_account.loginkey,PNR_LOGINKEY_MAXLEN) != OK)
        {
            ret_code = PNR_LOGIN_RETCODE_BAD_LOGINKEY;
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v2_deal:input loginkey(%s) real loginkey(%s)",account.loginkey,src_account.loginkey);
        }
        else
        {
            //查询是否已经存在的实例
            index = get_indexbytoxid(account.toxid);
            if(index)
            {
                if(g_imusr_array.usrnode[index].init_flag == FALSE)
                {
                    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"index(%d) init_flag(%d)",index,g_imusr_array.usrnode[index].init_flag);                
                    //g_tmp_instance_index = index;
                    if (pthread_create(&g_imusr_array.usrnode[index].tox_tid, NULL, imstance_daemon, &index) != 0) 
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                        ret_code = PNR_LOGIN_RETCODE_OTHERS;
                    } 
                    else
                    {
                        while(g_imusr_array.usrnode[index].init_flag != TRUE && run < PNR_TOXINSTANCE_CREATETIME)
                        {
                            sleep(1);
                            run++;
                        }
                        if(run >= PNR_TOXINSTANCE_CREATETIME)
                        {
                            ret_code = PNR_LOGIN_RETCODE_OTHERS;
                        }
                        else
                        {
                            *plws_index = index;
                            ret_code = PNR_LOGIN_RETCODE_OK;
                            DEBUG_PRINT(DEBUG_LEVEL_INFO,"renew user(%s)",g_imusr_array.usrnode[index].user_toxid);
                            //如果是新用户激活对应数据库句柄
                            if(g_msglogdb_handle[index] == NULL)
                            {
                                 if (sql_msglogdb_init(index) != OK) 
                                 {
                                     DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msglog db failed",index);
                                 }
                            }
                            if(g_msgcachedb_handle[index] == NULL)
                            {
                                 if (sql_msgcachedb_init(index) != OK) 
                                 {
                                     DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msgcache db failed",index);
                                 }
                            }
                        }
                    }
                }
                else
                {
                    *plws_index = index;
                    ret_code = PNR_LOGIN_RETCODE_OK;
                }
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"login bad uid(%s)",account.toxid);
                ret_code = PNR_LOGIN_RETCODE_BAD_UID;
            }
        }
    }
    //成功登陆
    if(ret_code == PNR_USER_LOGIN_OK)
    {
        //检测是否已经有用户登陆了，如果是，需要向之前用户推送消息
        if(g_imusr_array.usrnode[index].user_onlinestatus == USER_ONLINE_STATUS_ONLINE)
        {
            pnr_relogin_push(index,head->iftox,cur_friendnum,cur_pss);    
        }
        imuser_friendstatus_push(index,USER_ONLINE_STATUS_ONLINE);
        pnr_account_dbupdate_lastactive_bytoxid(g_imusr_array.usrnode[index].user_toxid);
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "user(%d-%s) online", index, 
            g_imusr_array.usrnode[index].user_toxid);
    }
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_LOGIN));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
#if (DB_CURRENT_VERSION < DB_VERSION_V3)
#else
    //暂时不用
    //cJSON_AddItemToObject(ret_params, "Index", cJSON_CreateString(g_imusr_array.usrnode[index].u_hashstr));
#endif
    cJSON_AddItemToObject(ret_params, "Routerid", cJSON_CreateString(router_id));
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));
    cJSON_AddItemToObject(ret_params, "NeedAsysn", cJSON_CreateNumber(need_asysn));
    if(ret_code == PNR_USER_LOGIN_OK)
    {
        cJSON_AddItemToObject(ret_params, "NickName", cJSON_CreateString(src_account.nickname));
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "NickName", cJSON_CreateString(""));
    }

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_login_identify_deal
  Description: IM模块用户登陆验证命令解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_login_identify_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct pnr_account_struct account;
    struct pnr_account_struct src_account;
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int datafile_version = 1;
    char datafile_paybuf[DATAFILE_BASE64_ENCODE_MAXLEN+1] = {0};
    char dstfile[PNR_FILEPATH_MAXLEN] = {0};
    int run =0;
    if(params == NULL)
    {
        return ERROR;
    }

    memset(&account,0,sizeof(account));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,PNR_USN_MAXLEN);    
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",account.toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"IdentifyCode",account.identifycode,PNR_IDCODE_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"LoginKey",account.loginkey,PNR_LOGINKEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",account.nickname,PNR_USERNAME_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"DataFileVersion",datafile_version,PNR_IDCODE_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"DataFilePay",datafile_paybuf,DATAFILE_BASE64_ENCODE_MAXLEN);
    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_LOGINIDENTIFY_RETCODE_BAD_RID;
    }
    else
    {
        //根据usn获取当前数据库中的账号信息
        memset(&src_account,0,sizeof(src_account));
        strcpy(src_account.user_sn,account.user_sn);
        pnr_account_get_byusn(&src_account);
        //只有未激活的账户才支持同步数据
        if(src_account.active == TRUE)
        {
            ret_code = PNR_LOGINIDENTIFY_RETCODE_USER_ACTIVE;
        }
        else if(src_account.type != PNR_USER_TYPE_ADMIN && src_account.type != PNR_USER_TYPE_NORMAL)
        {
            ret_code = PNR_LOGINIDENTIFY_RETCODE_BAD_USERTYPE;
        }
        else if(strncmp(account.identifycode,src_account.identifycode,PNR_IDCODE_MAXLEN) != OK)
        {
            ret_code = PNR_LOGINIDENTIFY_RETCODE_BAD_IDCODE;
        }
        else
        {
            snprintf(dstfile,PNR_FILEPATH_MAXLEN,"%s/user%d/%s",DAEMON_PNR_USERDATA_DIR,src_account.index,PNR_DATAFILE_DEFNAME);
            if(pnr_datafile_base64decode(dstfile,datafile_paybuf,strlen(datafile_paybuf)) != OK)
            {
                ret_code = PNR_LOGINIDENTIFY_RETCODE_BAD_DATAFILE;
            }
            else
            {
               //启动im server进程
               if (pthread_create(&g_imusr_array.usrnode[src_account.index].tox_tid, NULL, imstance_daemon, &src_account.index) != 0) 
               {
                   DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                   ret_code = PNR_LOGINIDENTIFY_RETCODE_BAD_DATAFILE;
               }  
               else
               {
                   while(g_imusr_array.usrnode[src_account.index].init_flag != TRUE && run < 5)
                   {
                       sleep(1);
                       run++;
                   }
                   if(run >= 5)
                   {
                       ret_code = PNR_LOGINIDENTIFY_RETCODE_BAD_DATAFILE;
                   }
                   else
                   {
                       *plws_index = src_account.type;
                       account.active = TRUE;
                       account.index = src_account.index;
                       account.type = src_account.type;
                       if(strncasecmp(account.toxid,g_imusr_array.usrnode[src_account.index].user_toxid,TOX_ID_STR_LEN) != OK)
                       {
                           ret_code = PNR_LOGINIDENTIFY_RETCODE_BAD_DATAFILE;
                           DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_login_identify_deal:try to create toxid(%s) but get(%s)",
                            account.toxid,g_imusr_array.usrnode[src_account.index].user_toxid);
                       }
                       else
                       {
                           pnr_account_dbupdate(&account);
                           ret_code = PNR_REGISTER_RETCODE_OK;
                       }
                   }
               }
            }
        }
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_LOGINIDENTIFY));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "Routerid", cJSON_CreateString(router_id));
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_logout_deal
  Description: IM模块用户登出解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_logout_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct pnr_account_struct account;
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    memset(&account,0,sizeof(account));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,PNR_USN_MAXLEN);    
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",account.toxid,TOX_ID_STR_LEN);
    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_LOGOUT_RETCODE_BADRID;
    }
    else if(strlen(account.toxid) != TOX_ID_STR_LEN)
    {
        ret_code = PNR_LOGOUT_RETCODE_BADUID;
    }
    else
    {
        index = get_indexbytoxid(account.toxid);
        if(index == 0)
        {
            ret_code = PNR_LOGOUT_RETCODE_BADUID;
        }
        else
        {
            ret_code = PNR_LOGOUT_RETCODE_OK;
        }
            
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_LOGOUT));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_routerlogin_deal
  Description: 管理用户登陆路由消息处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_routerlogin_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char mac_string[MACSTR_MAX_LEN+1] = {0};
    char loginkey[PNR_LOGINKEY_MAXLEN+1] = {0};
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Mac",mac_string,MACSTR_MAX_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"LoginKey",loginkey,PNR_LOGINKEY_MAXLEN);    
    //rid检查
    if(g_p2pnet_init_flag == FALSE)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_routerlogin_deal:g_p2pnet_init_flag not ready");
        ret_code = PNR_ROUTERLOGIN_RETCODE_BUSY;
    }
    else if((g_pnrdevtype ==PNR_DEV_TYPE_ONESPACE && (strncasecmp(mac_string,g_dev_hwaddr_full,MACSTR_MAX_LEN-1) != OK) && (strncasecmp(mac_string,g_dev1_hwaddr_full,MACSTR_MAX_LEN-1) != OK))
        || (g_pnrdevtype !=PNR_DEV_TYPE_ONESPACE && (strncasecmp(mac_string,g_dev_hwaddr_full,MACSTR_MAX_LEN-1) != OK)))
    {
        if(g_pnrdevtype ==PNR_DEV_TYPE_ONESPACE)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_routerlogin_deal onespace bad mac:input(%s) but real(%s,%s)",
                mac_string,g_dev_hwaddr_full,g_dev1_hwaddr_full);
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_routerlogin_deal bad mac:input(%s) but real(%s)",
                mac_string,g_dev_hwaddr_full);
        }
        ret_code = PNR_ROUTERLOGIN_RETCODE_BADMAC;
    }
    else if(strcmp(loginkey,g_devadmin_loginkey) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_routerlogin_deal bad loginkey:input(%s) but real(%s)",
            loginkey,g_devadmin_loginkey);
        ret_code = PNR_ROUTERLOGIN_RETCODE_BADKEY;
    }
    else
    {
        ret_code = PNR_ROUTERLOGIN_RETCODE_OK;
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_ROUTERLOGIN));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    if(ret_code == PNR_ROUTERLOGIN_RETCODE_OK)
    {
        cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(g_account_array.account[PNR_ADMINUSER_PSN_INDEX].user_sn));
        cJSON_AddItemToObject(ret_params, "IdentifyCode", cJSON_CreateString(g_account_array.account[PNR_ADMINUSER_PSN_INDEX].identifycode));
        cJSON_AddItemToObject(ret_params, "RouterId", cJSON_CreateString(g_daemon_tox.user_toxid));
        cJSON_AddItemToObject(ret_params, "Qrcode", cJSON_CreateString(g_account_array.defadmin_user_qrcode));
        if(strcmp(g_dev_nickname,DB_DEFAULT_DEVNAME_VALUE) != OK)
        {
            cJSON_AddItemToObject(ret_params, "RouterName", cJSON_CreateString(g_dev_nickname));
        }
    }

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_reset_routerkey_deal
  Description: 重置设备管理密码
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_reset_routerkey_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char old_key[PNR_LOGINKEY_MAXLEN+1] = {0};
    char new_key[PNR_LOGINKEY_MAXLEN+1] = {0};
    char router_id[TOX_ID_STR_LEN+1] = {0};
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"OldKey",old_key,PNR_LOGINKEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NewKey",new_key,PNR_LOGINKEY_MAXLEN);
    //rid检查
    if(strcmp(router_id,g_daemon_tox.user_toxid) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_reset_routerkey_deal bad rid:input(%s) but real(%s)",
            router_id,g_daemon_tox.user_toxid);
        ret_code = PNR_RESETLOGINKEY_RETCODE_BADRID;
    }
    else if(strcmp(old_key,g_devadmin_loginkey) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_routerlogin_deal bad loginkey:input(%s) but real(%s)",
            old_key,g_devadmin_loginkey);
        ret_code = PNR_RESETLOGINKEY_RETCODE_BADKEY;
    }
    else if(strlen(new_key) == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad input new_key null");
        ret_code = PNR_RESETLOGINKEY_RETCODE_BADKEY;
    }
    else
    {
        
        pnr_devloginkey_dbupdate(new_key);
        memset(g_devadmin_loginkey,0,PNR_LOGINKEY_MAXLEN);
        strcpy(g_devadmin_loginkey,new_key);
        ret_code = PNR_RESETLOGINKEY_RETCODE_OK;
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_RESETROUTERKEY));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_reset_useridcode_deal
  Description: 重置单个用户的激活码
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_reset_useridcode_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char old_idcode[PNR_IDCODE_MAXLEN+1] = {0};
    char new_idcode[PNR_IDCODE_MAXLEN+1] = {0};
    struct pnr_account_struct account;
    char router_id[TOX_ID_STR_LEN+1] = {0};
    if(params == NULL)
    {
        return ERROR;
    }
    memset(&account,0,sizeof(account));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,PNR_USN_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"OldCode",old_idcode,PNR_IDCODE_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NewCode",new_idcode,PNR_IDCODE_MAXLEN);
    //rid检查
    if(strcmp(router_id,g_daemon_tox.user_toxid) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_reset_routerkey_deal bad rid:input(%s) but real(%s)",
            router_id,g_daemon_tox.user_toxid);
        ret_code = PNR_RESETIDCODE_RETCODE_BADRID;
    }
    else if((strlen(account.user_sn) != PNR_USN_MAXLEN) || 
        (strlen(old_idcode) != PNR_IDCODE_MAXLEN) ||
        (strlen(new_idcode) != PNR_IDCODE_MAXLEN))
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_reset_routerkey_deal bad rid:input(%s:%s:%s)",
            account.user_sn,old_idcode,new_idcode);
        ret_code = PNR_RESETIDCODE_RETCODE_BADINPUT;

    }
    else 
    {
        pnr_account_get_byusn(&account);
        if(account.type == PNR_USER_TYPE_TEMP)
        {
            ret_code = PNR_RESETIDCODE_RETCODE_BADINPUT;
        }
        else if(strcmp(old_idcode,account.identifycode) != OK)
        {
            ret_code = PNR_RESETIDCODE_RETCODE_BADIDCODE;
        }
        else
        {
            ret_code = PNR_RESETIDCODE_RETCODE_OK;
            if(strcmp(old_idcode,new_idcode) != OK)
            {
                memset(g_account_array.account[account.index].identifycode,0,PNR_IDCODE_MAXLEN);
                strcpy(g_account_array.account[account.index].identifycode,new_idcode);
                strcpy(account.identifycode,new_idcode);
                pnr_account_dbupdate_idcode_byusn(&account);
            }
        }
    }
 
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_RESETUSERIDCODE));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_changeremarks_deal
  Description: IM模块用户修改好友备注
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_changeremarks_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,friend_id = 0;
    char user_toxid[TOX_ID_STR_LEN+1] = {0};
    char friend_toxid[TOX_ID_STR_LEN+1] = {0};
    char user_remarks[PNR_USERNAME_MAXLEN+1];
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",friend_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Remarks",user_remarks,PNR_USERNAME_MAXLEN);
    //rid检查
    if(strlen(user_toxid) != TOX_ID_STR_LEN || strlen(friend_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_changeremarks_deal:bad uid(%s) friend_id(%s)",user_toxid,friend_toxid);
        ret_code = PNR_CHANGEREMARKS_RETCODE_BADUID;
    }
    else
    {
        index = get_indexbytoxid(user_toxid);
        if(index == 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_changeremarks_deal:bad uid(%s)",user_toxid);   
            ret_code = PNR_CHANGEREMARKS_RETCODE_BADUID;
        }
        else 
        {
            friend_id = get_friendid_bytoxid(index,friend_toxid);
            if(friend_id < 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_changeremarks_deal:get friend(%s) failed",friend_toxid);   
                ret_code = PNR_CHANGEREMARKS_RETCODE_NOFRIEND;
            }
            else
            {
                ret_code = PNR_CHANGEREMARKS_RETCODE_OK;
                if(strcmp(user_remarks,g_imusr_array.usrnode[index].friends[friend_id].user_remarks) != OK)
                {
                    memset(g_imusr_array.usrnode[index].friends[friend_id].user_remarks,0,PNR_USERNAME_MAXLEN);
                    strcpy(g_imusr_array.usrnode[index].friends[friend_id].user_remarks,user_remarks);
                    pnr_friend_dbupdate_remarks_bytoxid(user_toxid,friend_toxid,user_remarks);
                }
            }
        }
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_CHANGEREMARKS));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(user_toxid));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(""));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_user_preregister_deal
  Description: IM模块PreRegister消息解析处理,现在没有使用
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_user_preregister_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char router_id[TOX_ID_STR_LEN+1] = {0};
    struct pnr_account_struct account;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    memset(&account,0,sizeof(account));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,TOX_ID_STR_LEN);

    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_LOGIN_RETCODE_BAD_RID;
    }
    else
    {
        //临时用户
        if(strcmp(account.user_sn,g_account_array.temp_user_sn) == OK)
        {
            if(g_imusr_array.cur_user_num < g_imusr_array.max_user_num)
            {
                ret_code = PNR_REGISTER_RETCODE_OK;
            }
            else
            {
                ret_code = PNR_REGISTER_RETCODE_OTHERS;
            }
        }
        else
        {
          //根据usn获取当前数据库中的账号信息
            pnr_account_get_byusn(&account);
            if(account.type < PNR_USER_TYPE_ADMIN || account.type >= PNR_USER_TYPE_BUTT)
            {
                ret_code = PNR_REGISTER_RETCODE_OTHERS;
                
            }
            else if(account.active == TRUE)
            {
                ret_code = PNR_REGISTER_RETCODE_USED;
            }
            else
            {
                ret_code = PNR_REGISTER_RETCODE_OK;
            }        
        }
    }

    //构建响应消息
	cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PREREGISTER));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserType", cJSON_CreateNumber(account.type));
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_user_register_deal
  Description: IM模块Register消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_user_register_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char router_id[TOX_ID_STR_LEN+1] = {0};
    struct pnr_account_struct account;
    struct pnr_account_struct src_account;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = PNR_REGISTER_RETCODE_OK;
    int need_synch = 0;
    char* ret_buff = NULL;
    int index = 0,run = 0;
    int temp_user_flag = FALSE;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    memset(&account,0,sizeof(account));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"IdentifyCode",account.identifycode,PNR_IDCODE_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"LoginKey",account.loginkey,PNR_LOGINKEY_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",account.nickname,PNR_USERNAME_MAXLEN);

    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_REGISTER_RETCODE_BADRID;
    }
    else
    {
        //临时用户
        if(strcmp(account.user_sn,g_account_array.temp_user_sn) == OK)
        {
            temp_user_flag = TRUE;
            if(g_imusr_array.cur_user_num >= g_imusr_array.max_user_num)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_user_register_deal: user(%d:%d) over",
                    g_imusr_array.cur_user_num,g_imusr_array.max_user_num);
                ret_code = PNR_REGISTER_RETCODE_OTHERS;
            }
        }
        else
        {
            memset(&src_account,0,sizeof(src_account));
            strcpy(src_account.user_sn,account.user_sn);
            //根据usn获取当前数据库中的账号信息
            pnr_account_get_byusn(&src_account);
            if(src_account.type < PNR_USER_TYPE_ADMIN || src_account.type >= PNR_USER_TYPE_BUTT)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%s) bad type(%d)",src_account.user_sn,src_account.type);
                ret_code = PNR_REGISTER_RETCODE_OTHERS;
            }
            else if(src_account.active == TRUE)
            {
                ret_code = PNR_REGISTER_RETCODE_USED;
            }
#if 0//暂时屏蔽
            //比对授权码
            else if(strcmp(src_account.identifycode,account.identifycode) != OK)
            {
                ret_code = PNR_REGISTER_RETCODE_BAD_IDCODE;
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_user_register_deal:input idcode(%s) but real idcode(%s)",
                    account.identifycode,src_account.identifycode);
            }
#endif            
        }
        if(ret_code == PNR_REGISTER_RETCODE_OK)
        {
           //如果当前还有可分配新用户
           if(g_imusr_array.cur_user_num < g_imusr_array.max_user_num)
           {
               for(index=1;index<=g_imusr_array.max_user_num;index++)
               {
                   if(g_imusr_array.usrnode[index].user_toxid[0] == 0)
                   {   
                       break;
                   }
               }
               if(index <= g_imusr_array.max_user_num)
               {
                   //启动im server进程
                   if (pthread_create(&g_imusr_array.usrnode[index].tox_tid, NULL, imstance_daemon, &index) != 0) 
                   {
                       DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                       ret_code = PNR_REGISTER_RETCODE_OTHERS;
                       need_synch = FALSE;
                   }  
                   else
                   {
                       while(g_imusr_array.usrnode[index].init_flag != TRUE && run < 5)
                       {
                           sleep(1);
                           run++;
                       }
                       if(run >= 5)
                       {
                           ret_code = PNR_REGISTER_RETCODE_OTHERS;
                           need_synch = FALSE;
                       }
                       else
                       {
                           *plws_index = index;
                           ret_code = PNR_REGISTER_RETCODE_OK;
                           need_synch = TRUE;
                           account.active = TRUE;
                           account.index = index;
                           pnr_account_gettype_byusn(account.user_sn,&account.type);
                           strcpy(account.toxid,g_imusr_array.usrnode[index].user_toxid);
                           if(temp_user_flag == TRUE)
                           {
                               pnr_account_tmpuser_dbinsert(&account);
                           }
                           else
                           {
                               pnr_account_dbupdate(&account);
                           }
                           strcpy(g_imusr_array.usrnode[index].user_nickname,account.nickname);
                           g_imusr_array.cur_user_num++;
                           //新用户注册激活对应数据库句柄
                           if(g_msglogdb_handle[index] == NULL)
                           {
                       			if (sql_msglogdb_init(index) != OK) 
                                {
                    				DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msglog db failed",index);
                    			}
                           }
			               if(g_msgcachedb_handle[index] == NULL)
                           {
                    			if (sql_msgcachedb_init(index) != OK) 
                                {
                    				DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msgcache db failed",index);
                    			}
                           }         
                       }
                   }
               }
               else
               {
                   DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get idle index failed");
                   ret_code = PNR_REGISTER_RETCODE_OTHERS;
                   need_synch = FALSE;
               }
           }
           else
           {
               DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_user_register_deal: user(%d:%d) over",
                    g_imusr_array.cur_user_num,g_imusr_array.max_user_num);
               ret_code = PNR_REGISTER_RETCODE_OTHERS;
               need_synch = FALSE;
           }
        }        
    }
    //成功注册
    if(ret_code == PNR_USER_LOGIN_OK)
    {
        imuser_friendstatus_push(index,USER_ONLINE_STATUS_ONLINE);
        pnr_account_dbupdate_lastactive_bytoxid(g_imusr_array.usrnode[index].user_toxid);
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "user(%d-%s) register online", index, 
            g_imusr_array.usrnode[index].user_toxid);
    }

    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_REGISTER));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "RouteId", cJSON_CreateString(router_id));
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    if(ret_code == PNR_REGISTER_RETCODE_OK)
    {
        //暂时不用
      /*if(head->api_version == PNR_API_VERSION_V3)
        {
            pnr_uidhash_get(index,0,g_imusr_array.usrnode[index].user_toxid,
                &g_imusr_array.usrnode[index].hashid,g_imusr_array.usrnode[index].u_hashstr);
            cJSON_AddItemToObject(ret_params, "Index", cJSON_CreateString(g_imusr_array.usrnode[index].u_hashstr));
        }*/
        cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));
        cJSON_AddItemToObject(ret_params, "DataFileVersion", cJSON_CreateNumber(ret_code));
        cJSON_AddItemToObject(ret_params, "DataFilePay", cJSON_CreateString(account.toxid));
        cJSON_AddItemToObject(ret_params, "RouterName", cJSON_CreateString(g_dev_nickname));
        cJSON_AddItemToObject(ret_params, "AdminId", cJSON_CreateString(g_imusr_array.usrnode[PNR_ADMINUSER_PSN_INDEX].user_toxid));
        cJSON_AddItemToObject(ret_params, "AdminName", cJSON_CreateString(g_imusr_array.usrnode[PNR_ADMINUSER_PSN_INDEX].user_nickname));
        cJSON_AddItemToObject(ret_params, "AdminKey", cJSON_CreateString(g_imusr_array.usrnode[PNR_ADMINUSER_PSN_INDEX].user_pubkey));
        pnr_account_dbupdate_lastactive_bytoxid(account.toxid);
    }

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_user_recovery_deal
  Description: IM模块PreRegister消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_user_recovery_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char router_id[TOX_ID_STR_LEN+1] = {0};
    struct pnr_account_struct account;
    struct pnr_account_struct src_account;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int datafile_version = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    memset(&account,0,sizeof(account));
    memset(&src_account,0,sizeof(src_account));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,PNR_USN_MAXLEN);
    if(head->api_version == PNR_API_VERSION_V4)
    {
        CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Pubkey",src_account.user_pubkey,PNR_USER_PUBKEY_MAXLEN);
    }

    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_RECOVERY_RETCODE_BAD_RID;
    }
    else if(head->api_version == PNR_API_VERSION_V4)
    {
        if(strlen(src_account.user_pubkey) > 0)
        {
            pnr_account_dbget_byuserkey(&src_account);
            if(src_account.active == FALSE)
            {
                //该用户未注册激活过 
                //临时用户直接返回
                if(strcmp(account.user_sn,g_account_array.temp_user_sn) == OK)
                {
                    ret_code = PNR_RECOVERY_RETCODE_TEMP_USER;
                }
                else
                {
                    //查看当前二维码账户是否已经被占用
                    pnr_account_get_byusn(&account);
                    if(account.active == FALSE)
                    {
                        ret_code = PNR_RECOVERY_RETCODE_USER_NOACTIVE;
                    }
                    else
                    {
                        ret_code = PNR_RECOVERY_RETCODE_QRCODE_USED;
                    }
                }
            }
            else//该用户pubkey已经注册过
            {
                ret_code = PNR_RECOVERY_RETCODE_USER_ACTIVED;
                if(strcmp(account.user_sn,src_account.user_sn) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_user_recovery_deal:input sn(%s) and regiseted sn(%s)",
                        account.user_sn,src_account.user_sn);
                }
                memcpy(&account,&src_account,sizeof(account));
            }
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_user_recovery_deal:input pubkey null");
            ret_code = PNR_RECOVERY_RETCODE_OTHERS_ERROR;
        }
    }
    else
    {    
        if(strcmp(account.user_sn,g_account_array.temp_user_sn) == OK)
        {
            ret_code = PNR_RECOVERY_RETCODE_TEMP_USER;
        }
        else
        {
            //根据usn获取当前数据库中的账号信息
            pnr_account_get_byusn(&account);
            //非临时账户只有已激活的账户才可以找回
            if(account.type < PNR_USER_TYPE_ADMIN || account.type >= PNR_USER_TYPE_BUTT)
            {
                ret_code = PNR_RECOVERY_RETCODE_OTHERS_ERROR;
            }
            else if(account.active == TRUE)
            {
                ret_code = PNR_RECOVERY_RETCODE_USER_ACTIVED;
            }
            else
            {
                ret_code = PNR_RECOVERY_RETCODE_USER_NOACTIVE;
            }        
        }
    }

    //构建响应消息
	cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_RECOVERY));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "RouteId", cJSON_CreateString(g_daemon_tox.user_toxid));
    if(strcmp(g_dev_nickname,DB_DEFAULT_DEVNAME_VALUE) != OK)
    {
        cJSON_AddItemToObject(ret_params, "RouterName", cJSON_CreateString(g_dev_nickname));
    }
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));
    cJSON_AddItemToObject(ret_params, "NickName", cJSON_CreateString(account.nickname));
    cJSON_AddItemToObject(ret_params, "DataFileVersion", cJSON_CreateNumber(datafile_version));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_user_recoveryidentify_deal
  Description: IM模块Recovery Identify消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_user_recoveryidentify_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char router_id[TOX_ID_STR_LEN+1] = {0};
    struct pnr_account_struct account;
    struct pnr_account_struct src_account;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char datafile_path[PNR_FILEPATH_MAXLEN+1] = {0};
    int datafile_version = 1;
    char datafile_paybuf[DATAFILE_BASE64_ENCODE_MAXLEN+1] = {0};
    int datafile_paylen = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    memset(&account,0,sizeof(account));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,PNR_USN_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"IdentifyCode",account.identifycode,PNR_IDCODE_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",account.toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"LoginKey",account.loginkey,PNR_LOGINKEY_MAXLEN);

    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_RECOVERYIDENTIFY_RETCODE_BADRID;
    }
    else
    {
        //临时用户,不支持找回
        if(strcmp(account.user_sn,g_account_array.temp_user_sn) == OK)
        {
            ret_code = PNR_RECOVERYIDENTIFY_RETCODE_NO_ACTIVE;
        }
        else
        {
            //根据usn获取当前数据库中的账号信息
            memset(&src_account,0,sizeof(src_account));
            strcpy(src_account.user_sn,account.user_sn);
            pnr_account_get_byusn(&src_account);
            //非临时账户只有已激活的账户才可以找回
            if(src_account.type < PNR_USER_TYPE_ADMIN || src_account.type >= PNR_USER_TYPE_BUTT)
            {
                ret_code = PNR_RECOVERYIDENTIFY_RETCODE_NO_ACTIVE;
            }
            else if(src_account.active != TRUE)
            {
                ret_code = PNR_RECOVERYIDENTIFY_RETCODE_NO_ACTIVE;
            }
            else if(strcmp(src_account.identifycode,account.identifycode) != OK)
            {
                ret_code = PNR_RECOVERYIDENTIFY_RETCODE_BAD_IDCODE;
            }   
            else if(strcmp(src_account.identifycode,account.identifycode) != OK)
            {
                ret_code = PNR_RECOVERYIDENTIFY_RETCODE_BAD_LOGINKEY;
            } 
            else
            {
                ret_code = PNR_RECOVERYIDENTIFY_RETCODE_OK;
            }            
        }
    }

    //构建响应消息
	cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_RECOVERYIDENTIFY));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));
    cJSON_AddItemToObject(ret_params, "DataFileVersion", cJSON_CreateNumber(datafile_version));
    if(ret_code == PNR_RECOVERYIDENTIFY_RETCODE_OK)
    {
        snprintf(datafile_path,PNR_FILEPATH_MAXLEN,"%s/user%d/%s",DAEMON_PNR_USERDATA_DIR,account.index,PNR_DATAFILE_DEFNAME);
        if(pnr_datafile_base64encode(datafile_path,datafile_paybuf,&datafile_paylen) == OK)
        {
            cJSON_AddItemToObject(ret_params, "DataFilePay", cJSON_CreateString(datafile_paybuf));
        }
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "DataFilePay", cJSON_CreateString(""));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
int pnr_readmsg_predeal(int index,char* tmp_msg,char* msg)
{
    char* tmp_msgid_head;
    char* msgid_buff_end;
    int send_dbid = 0;
    int tmp_dbid = 0;
    char send_dbid_str[IPSTR_MAX_LEN+1] = {0};
    if(index <=0 || index > PNR_IMUSER_MAXNUM || tmp_msg == NULL || msg == NULL)
    {
        return ERROR;
    }
    msg[0] = 0; 
    msgid_buff_end = tmp_msg + strlen(tmp_msg);
    tmp_msgid_head = tmp_msg;
    while(tmp_msgid_head != NULL)
    {
        tmp_dbid = atoi(tmp_msgid_head);
        if(tmp_dbid)
        {
            pnr_msglog_dbupdate_stauts_byid(index,tmp_dbid,MSG_STATUS_READ_OK);
            pnr_msglog_dbget_logid_byid(index,tmp_dbid,&send_dbid);
            memset(send_dbid_str,0,IPSTR_MAX_LEN);
            pnr_itoa(send_dbid,send_dbid_str);
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"get targetid(%d:%s)",send_dbid,send_dbid_str);
            if(msg[0] == 0)
            {
                strcpy(msg,send_dbid_str);
            }
            else
            {
                strcat(msg,",");
                strcat(msg,send_dbid_str);
            }
        }
        tmp_msgid_head = strchr(tmp_msgid_head,',');
        if(tmp_msgid_head)
        {
            tmp_msgid_head = tmp_msgid_head+1;
            if(tmp_msgid_head >= msgid_buff_end)
            {
                break;
            }
        }
        else
        {
            break;
        }
    }
    return OK;
}
/**********************************************************************************
  Function:      im_readmsg_cmd_deal
  Description: IM模块已阅通知消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_readmsg_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_sendmsg_msgstruct *msg;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
    char tmp_msgbuff[IM_MSG_MAXLEN+1] = {0};
    
    if (!params) {
        return ERROR;
    }

    msg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    head->forward = TRUE;

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",msg->touser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"ReadMsgs",tmp_msgbuff,IM_MSG_MAXLEN);
    msg->msgtype = PNR_IM_MSGTYPE_SYSTEM;

    //useid 处理
    if(strlen(msg->fromuser_toxid) != TOX_ID_STR_LEN
        || strlen(msg->touser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else if(strcmp(msg->fromuser_toxid,msg->touser_toxid) == OK)
    {
       DEBUG_PRINT(DEBUG_LEVEL_ERROR,"userid repeat(%s->%s)",
            msg->fromuser_toxid,msg->touser_toxid); 
       ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else if(strlen(tmp_msgbuff) > IM_MSG_PAYLOAD_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"msg too long(%d)",strlen(msg->msg_buff)); 
        ret_code = PNR_MSGSEND_RETCODE_FAILED;  
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(msg->fromuser_toxid);
        if(index == 0)
        {
            //清除对应记录
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get fromuser_toxid(%s) failed",msg->fromuser_toxid);
        }
        else
        {
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
            pnr_readmsg_predeal(index,tmp_msgbuff,msg->msg_buff);
            ret_code = PNR_USER_ADDFRIEND_RETOK;
            if (head->iftox) {
                head->toxmsg = msg;
                head->im_cmdtype = PNR_IM_CMDTYPE_READMSGPUSH;
                head->to_userid = get_indexbytoxid(msg->touser_toxid);
            } else {
                i = get_indexbytoxid(msg->touser_toxid);
                if(i != 0)
                {
                    im_pushmsg_callback(i,PNR_IM_CMDTYPE_READMSGPUSH,TRUE,head->api_version,(void *)msg);
                }
                else
                {
                    im_pushmsg_callback(index,PNR_IM_CMDTYPE_READMSGPUSH,FALSE,head->api_version,(void *)msg);
                }
            }
        }
    }
    
    //构建响应消息
    cJSON *ret_root = cJSON_CreateObject();
    cJSON *ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_READMSG));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateString(msg->msg_buff));

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);

    if (!head->iftox)
        free(msg);
    
    return OK;

ERR:
    if (!head->iftox)
        free(msg);
    
    return ERROR;
}
/**********************************************************************************
  Function:      im_userinfoupdate_cmd_deal
  Description: IM模块个人信息修改通知消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_userinfoupdate_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct im_friend_msgstruct *msg;
    struct pnr_account_struct account;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0;
    int changeflag = FALSE;
    int fr_id = 0,fr_index = 0;
    if (!params) {
        return ERROR;
    }

    msg = (struct im_friend_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    head->forward = TRUE;

    //解析参数
    memset(msg,0,sizeof(struct im_friend_msgstruct));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",msg->fromuser_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",msg->friend_nickname,PNR_USERNAME_MAXLEN);

    //useid 处理
    if(strlen(msg->fromuser_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s)",msg->fromuser_toxid);
        ret_code = PNR_USERINFOUPDATE_RETCODE_BADUID;
    }
    else
    {
        //根据toxid获取当前数据库中的账号信息
        memset(&account,0,sizeof(account));
        strcpy(account.toxid,msg->fromuser_toxid);
        pnr_account_dbget_byuserid(&account);
        index = get_indexbytoxid(account.toxid);
        if(account.active != TRUE || index == 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s) not active,index(%d)",account.toxid,index);
            ret_code = PNR_USERINFOUPDATE_RETCODE_BADUID;
        }
        else
        {
            //如果昵称有修改
            if(strcmp(msg->friend_nickname,account.nickname) != 0)
            {
                memset(account.nickname,0,PNR_USERNAME_MAXLEN);
                strcpy(account.nickname,msg->friend_nickname);
                changeflag = TRUE;
                memset(g_imusr_array.usrnode[index].user_nickname,0,PNR_USERNAME_MAXLEN);
                strcpy(g_imusr_array.usrnode[index].user_nickname,msg->friend_nickname);
            }
            if(changeflag == TRUE)
            {
                //更新自己的数据库
                pnr_account_dbupdate_bytoxid(&account);
                //遍历自己的好友,推送
                for(fr_id = 0; fr_id < PNR_IMUSER_FRIENDS_MAXNUM;fr_id++)
                {
                    if(g_imusr_array.usrnode[index].friends[fr_id].exsit_flag == TRUE)
                    {
                        strcpy(msg->touser_toxid,g_imusr_array.usrnode[index].friends[fr_id].user_toxid);
                        fr_index = get_indexbytoxid(g_imusr_array.usrnode[index].friends[fr_id].user_toxid);
                        if (head->iftox) 
                        {
                             head->toxmsg = msg;
                             head->im_cmdtype = PNR_IM_CMDTYPE_USERINFOUPDATE;
                             head->to_userid = get_indexbytoxid(msg->touser_toxid);
                        }
                        else
                        {
                            if(fr_index)
                            {
                                //如果是本地的，直接修改即可
                                im_update_friend_nickname(msg->touser_toxid,msg->fromuser_toxid,msg->friend_nickname);
                                im_pushmsg_callback(fr_index,PNR_IM_CMDTYPE_USERINFOPUSH,TRUE,head->api_version,(void *)msg);
                            }
                            else
                            {
                                im_pushmsg_callback(index,PNR_IM_CMDTYPE_USERINFOPUSH,FALSE,head->api_version,(void *)msg);
                            }
                        }
                    }
                }
            }
        }
    }
    
    //构建响应消息
    cJSON *ret_root = cJSON_CreateObject();
    cJSON *ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_USERINFOUPDATE));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(msg->fromuser_toxid));

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        goto ERR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);

    if (!head->iftox)
        free(msg);
    
    return OK;
ERR:
    if (!head->iftox)
        free(msg);
    
    return ERROR;
}


/**********************************************************************************
  Function:      im_pulluserlist_cmd_deal
  Description: IM模块拉取当前用户列表
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_pulluserlist_cmd_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    struct pnr_account_struct tmp_account;
    char start_usn[PNR_USN_MAXLEN+1] = {0};
    int need_num = 0;
    int need_type = 0;
    int tmp_account_num = 0;
    int normal_account_num = 0;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int i = 0;
    char **dbResult; 
    char *errmsg;
    int nRow, nColumn;
    int offset=0;
    char sql_cmd[SQL_CMD_LEN] = {0};
    int ret_len =0;
    char qrcode_buf[PNR_QRCODE_MAXLEN+1] = {0};
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserType",need_type,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserNum",need_num,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserStartSN",start_usn,PNR_USN_MAXLEN);
    //use_type检查
    if((need_type != 0) && ((need_type < PNR_USER_TYPE_ADMIN )||( need_type >= PNR_USER_TYPE_BUTT)))
    {
        ret_code = PNR_PULLACCOUNTLIST_RETCODE_BAD_USERTYPE;
    }
    else if(need_num != 0 && strlen(start_usn) != PNR_USN_MAXLEN && strlen(start_usn) <= 1)
    {
        ret_code = PNR_PULLACCOUNTLIST_RETCODE_BAD_USERSN;
    }
    else
    {
        ret_code = PNR_MSGSEND_RETCODE_OK;
    }   
    //构建响应消息
    cJSON *ret_root = cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V1));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLUSERLIST));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));

    if(ret_code == PNR_MSGSEND_RETCODE_OK)
    {
        if(need_num == 0)
        {
            if(need_type == 0)
            {
                snprintf(sql_cmd, SQL_CMD_LEN, "select * from user_account_tbl;");
            }
            else
            {
                snprintf(sql_cmd, SQL_CMD_LEN, "select * from user_account_tbl where type=%d;",need_type);
            }
        }
        else
        {
            if(need_type == 0)
            {
                if(strlen(start_usn) != PNR_USN_MAXLEN)
                {
                    snprintf(sql_cmd, SQL_CMD_LEN, "select * from user_account_tbl order by id desc limit %d;",need_num);
                }
                else
                {
                    snprintf(sql_cmd, SQL_CMD_LEN, "select * from user_account_tbl where id>(select id from user_account_tbl where UserSN='%s') order by id desc limit %d;",
                        start_usn,need_num);
                }
            }
            else
            {
                if(strlen(start_usn) != PNR_USN_MAXLEN)
                {
                    snprintf(sql_cmd, SQL_CMD_LEN, "select * from user_account_tbl where type=%d order by id desc limit %d;",need_type,need_num);
                }
                else
                {
                    snprintf(sql_cmd, SQL_CMD_LEN, "select * from user_account_tbl where type=%d and id>(select id from user_account_tbl where UserSN='%s') order by id desc limit %d;",
                        need_type,start_usn,need_num);
                }
            }
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "sql_cmd(%s)",sql_cmd);
        //add default tmp user
        pJsonsub = cJSON_CreateObject();
        if(pJsonsub == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
            cJSON_Delete(ret_root);
            return ERROR;
        }
        cJSON_AddItemToArray(pJsonArry,pJsonsub); 
        cJSON_AddStringToObject(pJsonsub,"UserSN",g_account_array.temp_user_sn);
        cJSON_AddNumberToObject(pJsonsub,"UserType",PNR_USER_TYPE_TEMP); 
        cJSON_AddNumberToObject(pJsonsub,"Active",FALSE); 
        cJSON_AddStringToObject(pJsonsub,"IdentifyCode","");
        cJSON_AddStringToObject(pJsonsub,"Mnemonic",PNR_TEMPUSER_MNEMONIC);
        cJSON_AddStringToObject(pJsonsub,"NickName","");
        cJSON_AddStringToObject(pJsonsub,"UserId","");
        cJSON_AddNumberToObject(pJsonsub,"LastLoginTime",0); 
        cJSON_AddStringToObject(pJsonsub,"Qrcode",g_account_array.temp_user_qrcode);
        //添加实际用户
        if(sqlite3_get_table(g_db_handle, sql_cmd, &dbResult, &nRow, 
            &nColumn, &errmsg) == SQLITE_OK)
        {
            offset = nColumn; //字段值从offset开始呀
            for( i = 0; i < nRow ; i++ )
            {               
                memset(&tmp_account,0,sizeof(tmp_account));
                //id = atoi(dbResult[offset]);
                tmp_account.lastactive = atoi(dbResult[offset+1]);
                tmp_account.type = atoi(dbResult[offset+2]);
                if(tmp_account.type == PNR_USER_TYPE_NORMAL)
                {
                    normal_account_num ++;
                }
                else if(tmp_account.type == PNR_USER_TYPE_TEMP)
                {
                    tmp_account_num ++;
                }
                tmp_account.active = atoi(dbResult[offset+3]);
                strncpy(tmp_account.identifycode,dbResult[offset+4],PNR_IDCODE_MAXLEN);
                strncpy(tmp_account.mnemonic,dbResult[offset+5],PNR_USERNAME_MAXLEN);
                strncpy(tmp_account.user_sn,dbResult[offset+6],PNR_USN_MAXLEN);
                if(tmp_account.active == TRUE)
                {
                    tmp_account.index = atoi(dbResult[offset+7]);
                    strncpy(tmp_account.nickname,dbResult[offset+8],PNR_USERNAME_MAXLEN);
                    strncpy(tmp_account.loginkey,dbResult[offset+9],PNR_LOGINKEY_MAXLEN);
                    strncpy(tmp_account.toxid,dbResult[offset+10],TOX_ID_STR_LEN);
                    //info和extinfo 跳过
                    strncpy(tmp_account.user_pubkey,dbResult[offset+13],PNR_USER_PUBKEY_MAXLEN);
                }
                else
                {
                    strncpy(tmp_account.nickname,tmp_account.mnemonic,PNR_USERNAME_MAXLEN);
                }
                tmp_account.createtime = atoi(dbResult[offset+14]);

                offset += nColumn;
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    return ERROR;
                }
                cJSON_AddItemToArray(pJsonArry,pJsonsub); 
                cJSON_AddStringToObject(pJsonsub,"UserSN",tmp_account.user_sn);
                cJSON_AddNumberToObject(pJsonsub,"UserType",tmp_account.type); 
                cJSON_AddNumberToObject(pJsonsub,"Active",tmp_account.active); 
                cJSON_AddStringToObject(pJsonsub,"IdentifyCode",tmp_account.identifycode);
                cJSON_AddStringToObject(pJsonsub,"Mnemonic",tmp_account.mnemonic);
                if(tmp_account.active == TRUE)
                {
                    cJSON_AddStringToObject(pJsonsub,"NickName",tmp_account.nickname);
                    cJSON_AddStringToObject(pJsonsub,"UserId",tmp_account.toxid);
                    cJSON_AddStringToObject(pJsonsub,"UserKey",tmp_account.user_pubkey);
                }
                else
                {
                    cJSON_AddStringToObject(pJsonsub,"NickName","");
                    cJSON_AddStringToObject(pJsonsub,"UserId","");
                }
                cJSON_AddNumberToObject(pJsonsub,"LastLoginTime",tmp_account.lastactive); 
                cJSON_AddNumberToObject(pJsonsub,"CreateTime",tmp_account.createtime); 
                if(tmp_account.type == PNR_USER_TYPE_TEMP)
                {
                    cJSON_AddStringToObject(pJsonsub,"Qrcode",g_account_array.temp_user_qrcode);
                }
                else
                {
                    memset(qrcode_buf,0,sizeof(qrcode_buf));
                    pnr_create_account_qrcode(tmp_account.user_sn,qrcode_buf,&ret_len);
                    cJSON_AddStringToObject(pJsonsub,"Qrcode",qrcode_buf);
                }
            }
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"get nRow(%d) nColumn(%d)",nRow,nColumn);
            sqlite3_free_table(dbResult);
        }
        cJSON_AddItemToObject(ret_params, "NormalUserNum", cJSON_CreateNumber(normal_account_num));
        cJSON_AddItemToObject(ret_params, "TempUserNum", cJSON_CreateNumber(tmp_account_num));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < VERSION_MAXLEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/*****************************************************************************
 函 数 名  : im_get_relationship_status_cmd_deal
 功能描述  : 查询好友关系状态
 输入参数  : cJSON *params                      
             char *retmsg                       
             int *retmsg_len                    
             int *plws_index                    
             struct imcmd_msghead_struct *head  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2019年1月3日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_get_relationship_status_cmd_deal(cJSON *params, char *retmsg, int *retmsg_len,
	int *plws_index, struct imcmd_msghead_struct *head)
{
	char fromid[TOX_ID_STR_LEN + 1] = {0};
	char toid[TOX_ID_STR_LEN + 1] = {0};
    char *tmp_json_buff = NULL;
    cJSON *tmp_item = NULL;
	cJSON *ret_root = NULL;
	cJSON *ret_params = NULL;
	int index = 0;
	int ret_code = 0;
	
	CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "UserId", fromid, TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params, tmp_item, tmp_json_buff, "FriendId", toid, TOX_ID_STR_LEN);

    index = get_indexbytoxid(fromid);
    if (index == 0) {
        ret_code = 2;
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "get fromid(%s) failed", fromid);
    } else {
        if (*plws_index == 0)
            *plws_index = index;

		if (!if_friend_available(index, toid)) {
			ret_code = 1;
		}
    }

	ret_root = cJSON_CreateObject();
    ret_params = cJSON_CreateObject();
    if (!ret_root || !ret_params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "err");
        cJSON_Delete(ret_root);
        goto ERR;
    }
	
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_QUERYFRIEND));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "FriendId", cJSON_CreateString(toid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);

    char *ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if (*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad ret(%d)", *retmsg_len);
        free(ret_buff);
        goto ERR;
    }
	
    strcpy(retmsg, ret_buff);
    free(ret_buff);

ERR:
    return OK;
}
/**********************************************************************************
  Function:      im_userlogin_v4_deal
  Description: IM模块用户登陆命令解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_userlogin_v4_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head, int cur_friendnum,
	struct per_session_data__minimal *cur_pss,int debugflag)
{
    struct pnr_account_struct account;
    struct pnr_account_struct src_account;
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char sign[PNR_AES_CBC_KEYSIZE+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int need_asysn = 0;
    int data_version = 0;
    int index =0;
    int run =0;
    if(params == NULL)
    {
        return ERROR;
    }

    memset(&account,0,sizeof(account));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,PNR_USN_MAXLEN);    
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",account.toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"DataFileVersion",data_version,PNR_IDCODE_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Sign",sign,PNR_AES_CBC_KEYSIZE);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",account.nickname,PNR_USERNAME_MAXLEN);
    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_LOGIN_RETCODE_BAD_RID;
    }
    else if(strlen(account.toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v4_deal:bad uid(%d:%s)",strlen(account.toxid),account.toxid);
        ret_code = PNR_LOGIN_RETCODE_BAD_UID;
    }
    else if(strlen(sign) == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v4_deal:bad input sign");
        ret_code = PNR_LOGIN_RETCODE_BAD_LOGINKEY;
    }
    else
    {
        //根据toxid获取当前数据库中的账号信息
        memset(&src_account,0,sizeof(src_account));
        strcpy(src_account.toxid,account.toxid);
        pnr_account_dbget_byuserid(&src_account);
        if(src_account.type < PNR_USER_TYPE_ADMIN || src_account.type >= PNR_USER_TYPE_BUTT)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad account type(%d)",src_account.type);
            ret_code = PNR_LOGIN_RETCODE_USER_INVAILD;
        }
        else if(src_account.active == FALSE)
        {
            ret_code = PNR_LOGIN_RETCODE_NEED_IDENTIFY;
        }
        //比较sn
        else if(strncasecmp(account.user_sn,src_account.user_sn,PNR_USN_MAXLEN) != OK)
        {
            ret_code = PNR_LOGIN_RETCODE_BAD_UID;
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v4_deal:input usn(%s) real usn(%s)",account.user_sn,src_account.user_sn);
        }
#if 0 //新版本不用loginkey验证改为pubkey
        else if(strncasecmp(account.loginkey,src_account.loginkey,PNR_LOGINKEY_MAXLEN) != OK)
        {
            ret_code = PNR_LOGIN_RETCODE_BAD_LOGINKEY;
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v2_deal:input loginkey(%s) real loginkey(%s)",account.loginkey,src_account.loginkey);
        }
#else
        //pubkey验证
        else if(debugflag != TRUE && pnr_sign_check(sign,strlen(sign),src_account.user_pubkey,TRUE) != OK)
        {
            ret_code = PNR_LOGIN_RETCODE_BAD_LOGINKEY;
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v4_deal:input sign(%s) usrkey(%s) check failed",sign,src_account.user_pubkey);
        }
        //检查并更新nickname
        else if((strlen(account.nickname) > 0) && (strncasecmp(account.nickname,src_account.nickname,PNR_USERNAME_MAXLEN) != OK))
        {
            if(pnr_account_dbupdate_dbinfo_bytoxid(&account) != OK)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_v4_deal:pnr_account_dbupdate_dbinfo_bytoxid err");
            }
        }
#endif        
        else
        {
            //查询是否已经存在的实例
            index = get_indexbytoxid(account.toxid);
            if(index)
            {
                if(g_imusr_array.usrnode[index].init_flag == FALSE)
                {
                    pthread_mutex_lock(&g_pnruser_lock[index]);
                    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"index(%d) init_flag(%d)",index,g_imusr_array.usrnode[index].init_flag);                
                    //g_tmp_instance_index = index;
                    if (pthread_create(&g_imusr_array.usrnode[index].tox_tid, NULL, imstance_daemon, &index) != 0) 
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                        ret_code = PNR_LOGIN_RETCODE_OTHERS;
                    } 
                    else
                    {
                        while(g_imusr_array.usrnode[index].init_flag != TRUE && run < PNR_TOXINSTANCE_CREATETIME)
                        {
                            sleep(1);
                            run++;
                        }
                        if(run >= PNR_TOXINSTANCE_CREATETIME)
                        {
                            ret_code = PNR_LOGIN_RETCODE_OTHERS;
                        }
                        else
                        {
                            *plws_index = index;
                            ret_code = PNR_LOGIN_RETCODE_OK;
                            DEBUG_PRINT(DEBUG_LEVEL_INFO,"renew user(%s)",g_imusr_array.usrnode[index].user_toxid);
                            //如果是新用户激活对应数据库句柄
                            if(g_msglogdb_handle[index] == NULL)
                            {
                                 if (sql_msglogdb_init(index) != OK) 
                                 {
                                     DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msglog db failed",index);
                                 }
                            }
                            if(g_msgcachedb_handle[index] == NULL)
                            {
                                 if (sql_msgcachedb_init(index) != OK) 
                                 {
                                     DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msgcache db failed",index);
                                 }
                            }
                        }
                    }
                    pthread_mutex_unlock(&g_pnruser_lock[index]);
                }
                else
                {
                    *plws_index = index;
                    ret_code = PNR_LOGIN_RETCODE_OK;
                }
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"login bad uid(%s)",account.toxid);
                ret_code = PNR_LOGIN_RETCODE_BAD_UID;
            }
        }
    }
    //成功登陆
    if(ret_code == PNR_USER_LOGIN_OK)
    {
        //检测是否已经有用户登陆了，如果是，需要向之前用户推送消息
        if(g_imusr_array.usrnode[index].user_onlinestatus == USER_ONLINE_STATUS_ONLINE)
        {
            pnr_relogin_push(index,head->iftox,cur_friendnum,cur_pss);    
        }
        imuser_friendstatus_push(index,USER_ONLINE_STATUS_ONLINE);
        pnr_account_dbupdate_lastactive_bytoxid(g_imusr_array.usrnode[index].user_toxid);
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "user(%d-%s) online", index, 
            g_imusr_array.usrnode[index].user_toxid);
    }
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_LOGIN));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "Routerid", cJSON_CreateString(router_id));
    cJSON_AddItemToObject(ret_params, "RouterName", cJSON_CreateString(g_dev_nickname));
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));
    cJSON_AddItemToObject(ret_params, "NeedAsysn", cJSON_CreateNumber(need_asysn));
    if(ret_code == PNR_USER_LOGIN_OK)
    {
        cJSON_AddItemToObject(ret_params, "NickName", cJSON_CreateString(account.nickname));
        cJSON_AddItemToObject(ret_params, "AdminId", cJSON_CreateString(g_imusr_array.usrnode[PNR_ADMINUSER_PSN_INDEX].user_toxid));
        cJSON_AddItemToObject(ret_params, "AdminName", cJSON_CreateString(g_imusr_array.usrnode[PNR_ADMINUSER_PSN_INDEX].user_nickname));
        cJSON_AddItemToObject(ret_params, "AdminKey", cJSON_CreateString(g_imusr_array.usrnode[PNR_ADMINUSER_PSN_INDEX].user_pubkey));
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "NickName", cJSON_CreateString(""));
    }

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_user_register_v4_deal
  Description: IM模块Register消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_user_register_v4_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char sign[PNR_AES_CBC_KEYSIZE+1] = {0};
    struct pnr_account_struct account;
    struct pnr_account_struct src_account;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = PNR_REGISTER_RETCODE_OK;
    int need_synch = 0;
    char* ret_buff = NULL;
    int index = 0,run = 0;
    int temp_user_flag = FALSE;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    memset(&account,0,sizeof(account));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",account.nickname,PNR_USERNAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Sign",sign,PNR_AES_CBC_KEYSIZE);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Pubkey",account.user_pubkey,PNR_USER_PUBKEY_MAXLEN);
    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_REGISTER_RETCODE_BADRID;
    }
    else if(strlen(sign) == 0 || strlen(account.user_pubkey) == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad input");
        ret_code = PNR_REGISTER_RETCODE_OTHERS;
    }
    else if(pnr_account_dbcheck_bypubkey(&account) == ERROR)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user exsit");
        ret_code = PNR_REGISTER_RETCODE_USED;
    }
    else
    {
        //临时用户
        if(strcmp(account.user_sn,g_account_array.temp_user_sn) == OK)
        {
            temp_user_flag = TRUE;
            if(g_imusr_array.cur_user_num >= g_imusr_array.max_user_num)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_user_register_deal: user(%d:%d) over",
                    g_imusr_array.cur_user_num,g_imusr_array.max_user_num);
                ret_code = PNR_REGISTER_RETCODE_OTHERS;
            }
        }
        else
        {
            memset(&src_account,0,sizeof(src_account));
            strcpy(src_account.user_sn,account.user_sn);
            //根据usn获取当前数据库中的账号信息
            pnr_account_get_byusn(&src_account);
            if(src_account.type < PNR_USER_TYPE_ADMIN || src_account.type >= PNR_USER_TYPE_BUTT)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%s) bad type(%d)",src_account.user_sn,src_account.type);
                ret_code = PNR_REGISTER_RETCODE_OTHERS;
            }
            else if(src_account.active == TRUE)
            {
                ret_code = PNR_REGISTER_RETCODE_USED;
            }
#if 0//暂时屏蔽
            //比对授权码
            else if(strcmp(src_account.identifycode,account.identifycode) != OK)
            {
                ret_code = PNR_REGISTER_RETCODE_BAD_IDCODE;
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_user_register_deal:input idcode(%s) but real idcode(%s)",
                    account.identifycode,src_account.identifycode);
            }
#endif            
        }
        if(ret_code == PNR_REGISTER_RETCODE_OK)
        {
           //如果当前还有可分配新用户
           if(g_imusr_array.cur_user_num < g_imusr_array.max_user_num)
           {
               for(index=1;index<=g_imusr_array.max_user_num;index++)
               {
                   if(g_imusr_array.usrnode[index].user_toxid[0] == 0)
                   {   
                       break;
                   }
               }
               //验证pubkey
               if(index <= g_imusr_array.max_user_num)
               {
                   pthread_mutex_lock(&g_pnruser_lock[index]);
                   //启动im server进程
                   if (pthread_create(&g_imusr_array.usrnode[index].tox_tid, NULL, imstance_daemon, &index) != 0) 
                   {
                       DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                       ret_code = PNR_REGISTER_RETCODE_OTHERS;
                       need_synch = FALSE;
                   }  
                   else
                   {
                       while(g_imusr_array.usrnode[index].init_flag != TRUE && run < PNR_TOXINSTANCE_CREATETIME)
                       {
                           sleep(1);
                           run++;
                       }
                       if(run >= PNR_TOXINSTANCE_CREATETIME)
                       {
                           DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create instance err");
                           ret_code = PNR_REGISTER_RETCODE_OTHERS;
                           need_synch = FALSE;
                       }
                       else
                       {
                           *plws_index = index;
                           ret_code = PNR_REGISTER_RETCODE_OK;
                           need_synch = TRUE;
                           account.active = TRUE;
                           account.index = index;
                           pnr_account_gettype_byusn(account.user_sn,&account.type);
                           strcpy(account.toxid,g_imusr_array.usrnode[index].user_toxid);
                           if(temp_user_flag == TRUE)
                           {
                               pnr_account_tmpuser_dbinsert(&account);
                           }
                           else
                           {
                               pnr_account_dbupdate(&account);
                           }
                           //注册激活的时候记录一下
                           //pnr_logcache_dbinsert(PNR_IM_CMDTYPE_REGISTER,account.toxid,account.toxid,PNR_CMDTYPE_MSG_REGISTER##account.nickname,PNR_CMDTYPE_EXT_SUCCESS);
                           strcpy(g_imusr_array.usrnode[index].user_nickname,account.nickname);
                           g_imusr_array.cur_user_num++;
                           //新用户注册激活对应数据库句柄
                           if(g_msglogdb_handle[index] == NULL)
                           {
                       			if (sql_msglogdb_init(index) != OK) 
                                {
                    				DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msglog db failed",index);
                    			}
                           }
			               if(g_msgcachedb_handle[index] == NULL)
                           {
                    			if (sql_msgcachedb_init(index) != OK) 
                                {
                    				DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msgcache db failed",index);
                    			}
                           }         
                       }
                   }
                   pthread_mutex_unlock(&g_pnruser_lock[index]);
               }
               else
               {
                   DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get idle index failed");
                   ret_code = PNR_REGISTER_RETCODE_OTHERS;
                   need_synch = FALSE;
               }
           }
           else
           {
               DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_user_register_deal: user(%d:%d) over",
                    g_imusr_array.cur_user_num,g_imusr_array.max_user_num);
               ret_code = PNR_REGISTER_RETCODE_OTHERS;
               need_synch = FALSE;
           }
        }        
    }
    //成功注册
    if(ret_code == PNR_USER_LOGIN_OK)
    {
        imuser_friendstatus_push(index,USER_ONLINE_STATUS_ONLINE);
        pnr_account_dbupdate_lastactive_bytoxid(g_imusr_array.usrnode[index].user_toxid);
        //自动添加admin用户为好友
        if(strcmp(account.user_sn,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].user_sn) != OK)
        {
            if(g_account_array.admin_user_index == 0)
            {
                memset(&src_account,0,sizeof(src_account));
                strcpy(src_account.user_sn,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].user_sn);
                pnr_account_get_byusn(&src_account);
                if(src_account.active == TRUE && src_account.index > 0)
                {
                    g_account_array.admin_user_index = src_account.index;
                }
            }
            if(g_account_array.admin_user_index > 0)
            {
                pnr_autoadd_localfriend(index,g_account_array.admin_user_index,&account);
            }
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO, "user(%d-%s) register online", index, 
            g_imusr_array.usrnode[index].user_toxid);
    }

    //构建响应消息
	cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_REGISTER));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "RouteId", cJSON_CreateString(router_id));
    cJSON_AddItemToObject(ret_params, "RouterName", cJSON_CreateString(g_dev_nickname));
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    if(ret_code == PNR_REGISTER_RETCODE_OK)
    {
        //暂时不用
      /*if(head->api_version == PNR_API_VERSION_V3)
        {
            pnr_uidhash_get(index,0,g_imusr_array.usrnode[index].user_toxid,
                &g_imusr_array.usrnode[index].hashid,g_imusr_array.usrnode[index].u_hashstr);
            cJSON_AddItemToObject(ret_params, "Index", cJSON_CreateString(g_imusr_array.usrnode[index].u_hashstr));
        }*/
        cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));
        cJSON_AddItemToObject(ret_params, "DataFileVersion", cJSON_CreateNumber(ret_code));
        cJSON_AddItemToObject(ret_params, "DataFilePay", cJSON_CreateString(account.toxid));
        pnr_account_dbupdate_lastactive_bytoxid(account.toxid);
    }

    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_pullfriend_cmd_deal_v4
  Description: IM模块拉取好友信息消息处理V4版本
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_pullfriend_cmd_deal_v4(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char user_toxid[TOX_ID_STR_LEN+1] = {0};
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int index = 0,i = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_toxid,TOX_ID_STR_LEN);

    //useid 处理
    if(strlen(user_toxid) != TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%s)",user_toxid);
        ret_code = PNR_MSGSEND_RETCODE_FAILED;
    }
    else
    {
        //查询是否已经存在的实例
        index = get_indexbytoxid(user_toxid);
        if(index == 0)
        {
            ret_code = PNR_MSGSEND_RETCODE_FAILED;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"get UserId(%s) failed",user_toxid);
        }
        else
        {
            ret_code = PNR_MSGSEND_RETCODE_OK;
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
        }
    }
    
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)(head->api_version)));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));
	cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLFRIEDN));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    if(ret_code == PNR_MSGSEND_RETCODE_OK && index != 0
        && g_imusr_array.usrnode[index].friendnum > 0)
    {
        pthread_mutex_lock(&(g_user_friendlock[index]));
        cJSON_AddItemToObject(ret_params, "FriendNum", cJSON_CreateNumber(g_imusr_array.usrnode[index].friendnum));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
        for(i=0;i<PNR_IMUSER_FRIENDS_MAXNUM;i++)
        {
            if(g_imusr_array.usrnode[index].friends[i].exsit_flag)
            {
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    pthread_mutex_unlock(&(g_user_friendlock[index]));
                    return ERROR;
                }
           		cJSON_AddItemToArray(pJsonArry,pJsonsub); 
#if 0//暂时不用
        		cJSON_AddStringToObject(pJsonsub,"Index", g_imusr_array.usrnode[index].friends[i].u_hashstr);
#endif
                cJSON_AddStringToObject(pJsonsub,"Name", g_imusr_array.usrnode[index].friends[i].user_nickname);
        		cJSON_AddStringToObject(pJsonsub,"Remarks", g_imusr_array.usrnode[index].friends[i].user_remarks);
        		cJSON_AddStringToObject(pJsonsub,"Id", g_imusr_array.usrnode[index].friends[i].user_toxid);
        		cJSON_AddStringToObject(pJsonsub,"RouteId", g_imusr_array.usrnode[index].friends[i].user_devid);
        		cJSON_AddStringToObject(pJsonsub,"UserKey", g_imusr_array.usrnode[index].friends[i].user_pubkey);
        		cJSON_AddNumberToObject(pJsonsub,"Status",g_imusr_array.usrnode[index].friends[i].online_status); 
        		//cJSON_AddStringToObject(pJsonsub,"RouteName", g_imusr_array.usrnode[index].friends[i].user_devname);
                if(strcmp(g_imusr_array.usrnode[index].friends[i].user_devid,g_daemon_tox.user_toxid) != OK)
                {
                    if(strcmp(g_imusr_array.usrnode[index].friends[i].user_devname,g_dev_nickname) == OK)
                    {
                        struct im_userdev_mapping_struct devinfo;
                        memset(&devinfo,0,sizeof(devinfo));
                        strcpy(devinfo.user_toxid,g_imusr_array.usrnode[index].friends[i].user_toxid);
                        if(pnr_usrdev_mappinginfo_sqlget(&devinfo) != OK)
                        {
                            DEBUG_PRINT(DEBUG_LEVEL_INFO,"###im_pullfriend_cmd_deal_v4:pnr_usrdev_mappinginfo_sqlget failed");
                        }
                        else
                        {
                            cJSON_AddStringToObject(pJsonsub,"RouteName", devinfo.user_devname);
                            memset(g_imusr_array.usrnode[index].friends[i].user_devname,0,PNR_USERNAME_MAXLEN);
                            strcpy(g_imusr_array.usrnode[index].friends[i].user_devname,devinfo.user_devname);
                        }
                    }
                    else
                    {
                        cJSON_AddStringToObject(pJsonsub,"RouteName", g_imusr_array.usrnode[index].friends[i].user_devname);
                    }
                }
                else
                {
                    cJSON_AddStringToObject(pJsonsub,"RouteName", g_imusr_array.usrnode[index].friends[i].user_devname);
                }
                /*DEBUG_PRINT(DEBUG_LEVEL_INFO,"get friend(%d:%s:%s:%s:%s)",
                        i,g_imusr_array.usrnode[index].friends[i].user_nickname,
                        g_imusr_array.usrnode[index].friends[i].user_remarks,
                        g_imusr_array.usrnode[index].friends[i].user_toxid,
                        g_imusr_array.usrnode[index].friends[i].user_devname);*/
            }
        }
        pthread_mutex_unlock(&(g_user_friendlock[index]));
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "FriendNum", cJSON_CreateNumber(0));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < VERSION_MAXLEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_reset_routername_deal
  Description: 重置设备昵称
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_reset_routername_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char new_name[PNR_USERNAME_MAXLEN+1] = {0};
    char router_id[TOX_ID_STR_LEN+1] = {0};
    char user_id[TOX_ID_STR_LEN+1] = {0};
    int i = 0 ,j =0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Name",new_name,PNR_USERNAME_MAXLEN);
    //rid检查
    if(strcmp(router_id,g_daemon_tox.user_toxid) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_reset_routername_deal bad rid:input(%s) but real(%s)",
            router_id,g_daemon_tox.user_toxid);
        ret_code = PNR_RESETDEVNAME_RETCODE_BADRID;
    }
    //这里可能还没有初始化admin用户
#if 0
    else if((g_account_array.account[PNR_ADMINUSER_PSN_INDEX].active == TRUE) 
            &&(strcmp(user_id,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].toxid) != OK))
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_reset_routername_deal bad user_toxid:input(%s) but real(%s)",
            user_id,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].toxid);
        ret_code = PNR_RESETDEVNAME_RETCODE_BADUID;
    }
#endif
    else if(strlen(new_name) == 0 || strlen(new_name) > PNR_USERNAME_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad input new_key len(%d)", strlen(new_name));
        ret_code = PNR_RESETDEVNAME_RETCODE_OTHERS;
    }
    else
    {
        if(strcmp(new_name,g_dev_nickname) != OK)
        {
            pnr_devname_dbupdate(new_name);
            pnr_userdev_mapping_dbupdate_bydevid(g_daemon_tox.user_toxid,new_name);
            memset(g_dev_nickname,0,PNR_USERNAME_MAXLEN);
            strcpy(g_dev_nickname,new_name);
            //遍历更新本地好友关系缓存中所有本地用户的设备名称
            for(i = 0; i< PNR_IMUSER_MAXNUM;i++)
            {
                if(g_imusr_array.usrnode[i].init_flag == TRUE)
                {
                    for(j = 0;j < PNR_IMUSER_FRIENDS_MAXNUM; j++)
                    {
                        if(g_imusr_array.usrnode[i].friends[j].exsit_flag == TRUE)
                        {
                            memset(g_imusr_array.usrnode[i].friends[j].user_devname,0,PNR_USERNAME_MAXLEN);
                            strcpy(g_imusr_array.usrnode[i].friends[j].user_devname,new_name);
                        }
                    }
                }
            }
            //遍历发送通知其他路由器上相关用户
        }
        ret_code = PNR_RESETLOGINKEY_RETCODE_OK;
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_RESETROUTERNAME));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_group_usermap_analyze
  Description: 群用户和群用户密钥映射解析
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_usermap_analyze(char* fidlist,char* fkeylist,struct gpuser_maplist* plist)
{
    int usernum = 0;
    int keynum = 0;
    char* tmp = NULL;
    char* tmp_end = NULL;
    char* value_end = NULL;
    int value_len = 0;
    if( fidlist == NULL || fkeylist == NULL || plist == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_usermap_analyze:input params err");
        return ERROR;
    }
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_group_usermap_analyze:in(%s:%s)",fidlist,fkeylist);
    //memset(plist,0,sizeof(struct gpuser_maplist));
    tmp = fidlist;
    value_end = fidlist+strlen(fidlist);
    while(tmp < value_end)
    {
        tmp_end=strchr(tmp,',');
        if(tmp_end == NULL)
        {
            value_len = value_end - tmp;
        }
        else
        {
            value_len = tmp_end - tmp;
            tmp_end++;
        }
        if(value_len <= TOX_ID_STR_LEN && value_len > 0)
        {
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_group_usermap_analyze:get user(%d:%s)",value_len,tmp);
            strncpy(plist->gpuser[usernum].userid,tmp,((value_len >=TOX_ID_STR_LEN)?TOX_ID_STR_LEN:value_len));
            plist->gpuser[usernum].uindex = get_indexbytoxid(plist->gpuser[usernum].userid);
            if(plist->gpuser[usernum].uindex == 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_usermap_analyze:user(%s) not found",plist->gpuser[usernum].userid);
                return ERROR;
            }
            usernum++;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_group_usermap_analyze:get user(%d:%d)",usernum,plist->gpuser[usernum].uindex);
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_usermap_analyze:user(%s) len error",tmp);
            return ERROR;
        }
        if(tmp_end < value_end && tmp_end != NULL)
        {
            tmp = tmp_end;
        }
        else
        {
            break;
        }
    }
    tmp = fkeylist;
    value_end = fkeylist+strlen(fkeylist);
    while(tmp < value_end)
    {
        tmp_end=strchr(tmp,',');
        if(tmp_end == NULL)
        {
            value_len = value_end - tmp;
        }
        else
        {
            value_len = tmp_end - tmp;
            tmp_end++;
        }
        if(value_len <= PNR_GROUP_USERKEY_MAXLEN && value_len > 0)
        {
            strncpy(plist->gpuser[keynum].userkey,tmp,value_len);
            keynum++;
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_usermap_analyze:key(%s) len error",tmp);
            return ERROR;
        }
        if(tmp_end < value_end && tmp_end != NULL)
        {
            tmp = tmp_end;
        }
        else
        {
            break;
        }
    }
    if(keynum != usernum || usernum == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_usermap_analyze:get key(%d) user(%d)",keynum,usernum);
        return ERROR;
    }
    plist->usernum = usernum;
    return OK;
}
/**********************************************************************************
  Function:      im_group_fileinfo_analyze
  Description: 群消息记录文件信息解析
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_fileinfo_analyze(char* fileinfo,struct group_fileinfo_struct* pfinfo)
{
    char* tmp = NULL;
    char* tmp_end = NULL;
    char* info_end = NULL;
    char tmp_fileinfo[PNR_GROUP_EXTINFO_MAXLEN+1] = {0};
    if( fileinfo == NULL || pfinfo == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_fileinfo_analyze:input params err");
        return ERROR;
    }
    strncpy(tmp_fileinfo,fileinfo,PNR_GROUP_EXTINFO_MAXLEN);
    //fileinfo格式为  fid:fsize:fmd5:finfo   最后的finfo可能有，可能没有
    tmp = tmp_fileinfo;
    info_end = tmp_fileinfo+strlen(tmp_fileinfo);
    tmp_end=strchr(tmp,':');
    if(tmp_end)
    {
        tmp_end[0] = 0;
        strncpy(pfinfo->fileid,tmp,PNR_FILEINFO_MAXLEN);
        tmp = tmp_end+1;
        tmp_end = strchr(tmp,':');
        if(tmp_end)
        {
            tmp_end[0] = 0;
            pfinfo->filesize = atoi(tmp);
            tmp = tmp_end+1;
            tmp_end = strchr(tmp,':');
            if(tmp_end)
            {
                tmp_end[0] = 0;
                strncpy(pfinfo->md5,tmp,PNR_MD5_VALUE_MAXLEN);
                tmp = tmp_end+1;
                if(tmp && tmp < info_end)
                {
                    strncpy(pfinfo->attach_info,tmp,PNR_FILEINFO_MAXLEN);
                }
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_group_fileinfo_analyze:fileinfo(%s) analyze(%s:%d:%s:%s)",
                    fileinfo,pfinfo->fileid,pfinfo->filesize,pfinfo->md5,pfinfo->attach_info);
                return OK;
            }
            else if(tmp && tmp < info_end)
            {
                strncpy(pfinfo->md5,tmp,PNR_MD5_VALUE_MAXLEN);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_group_fileinfo_analyze:fileinfo(%s) analyze(%s:%d:%s)",
                    fileinfo,pfinfo->fileid,pfinfo->filesize,pfinfo->md5);
                return OK;
            }
        }
    }
    return ERROR;
}

/**********************************************************************************
  Function:      pnr_group_create
  Description: 创建群组初始化
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int pnr_group_create(int gid,int ownerid,int verify,char* groupname,char* owner)
{
    int i= 0;
    if(gid < 0 || gid >= PNR_GROUP_MAXNUM || groupname == NULL || owner == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_adduser:input params err");
        return ERROR;
    }
    pthread_mutex_lock(&g_grouplock[gid]);
    if(g_grouplist[gid].init_flag == TRUE)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_create:gid(%d) already init",gid);
        pthread_mutex_unlock(&g_grouplock[gid]);
        return ERROR;
    }
    memset(&g_grouplist[gid],0,sizeof(struct group_info));
    strcpy(g_grouplist[gid].group_name,groupname);
    snprintf(g_grouplist[gid].group_hid,TOX_ID_STR_LEN,"%s%d_admin%d_time%d",PNR_GROUPID_PREFIX,gid,ownerid,(int)time(NULL));
    for(i=strlen(g_grouplist[gid].group_hid);i<TOX_ID_STR_LEN;i++)
    {
        g_grouplist[gid].group_hid[i] = 'A';
    }
    g_grouplist[gid].group_hid[TOX_ID_STR_LEN] = '\0';
    strcpy(g_grouplist[gid].owner,owner);
    g_grouplist[gid].ownerid = ownerid;
    g_grouplist[gid].verify = verify;
    g_grouplist[gid].init_flag = TRUE;
    snprintf(g_grouplist[gid].group_filepath,PNR_FILEPATH_MAXLEN,"%s%sg%d",DAEMON_PNR_USERDATA_DIR,PNR_GROUP_DATA_PATH,gid);
    //插入数据库
    pnr_group_dbinsert(gid,ownerid,verify,owner,groupname,g_grouplist[gid].group_hid);
    pthread_mutex_unlock(&g_grouplock[gid]);
    return OK;
}

/**********************************************************************************
  Function:      pnr_group_adduser
  Description: 添加群组成员
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int pnr_group_adduser(int gid,int user_type,char* user_toxid,char* userkey)
{
    int uindex = 0;
    int u_id = 0;
    struct group_user* p_exsit = NULL;
    struct pnr_account_struct account;
    if(gid < 0 || gid >= PNR_GROUP_MAXNUM || user_toxid == NULL || userkey == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_adduser:input params err");
        return ERROR;
    }
    pthread_mutex_lock(&g_grouplock[gid]);
    p_exsit= get_guserbytoxid(gid,user_toxid);
    if(p_exsit != NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_adduser:group(%d) user(%s) exsit",gid,user_toxid);
        pthread_mutex_unlock(&g_grouplock[gid]);
        return OK;
    }
    if(g_grouplist[gid].user_num > PNR_GROUP_USER_MAXNUM)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_adduser:group(%d) user over",gid);
        pthread_mutex_unlock(&g_grouplock[gid]);
        return ERROR;
    }
    for(u_id=1;u_id <= PNR_GROUP_USER_MAXNUM;u_id++)
    {
        //找到一个空的
        if(g_grouplist[gid].user[u_id].userindex==0)
        {
            break;
        }
    }
    if(u_id > PNR_GROUP_USER_MAXNUM)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_adduser:group(%d) user over",gid);
        pthread_mutex_unlock(&g_grouplock[gid]);
        return ERROR;
    }
    memset(&g_grouplist[gid].user[u_id],0,sizeof(struct group_user));
    g_grouplist[gid].user[u_id].type = user_type;
    g_grouplist[gid].user[u_id].gindex = u_id;
    g_grouplist[gid].user[u_id].init_msgid = g_grouplist[gid].last_msgid;
    g_grouplist[gid].user[u_id].last_msgid = g_grouplist[gid].last_msgid;
    strcpy(g_grouplist[gid].user[u_id].userkey,userkey);
    strcpy(g_grouplist[gid].user[u_id].toxid,user_toxid);
    uindex = get_indexbytoxid(user_toxid);
    if(uindex)
    {
        g_grouplist[gid].user[u_id].userindex = uindex;
    }
    memset(&account,0,sizeof(account));
    strcpy(account.toxid,user_toxid);
    pnr_account_dbget_byuserid(&account);
    if(account.user_pubkey[0] != 0)
    {
        strcpy(g_grouplist[gid].user[u_id].user_pubkey,account.user_pubkey);
    }
    if(account.nickname[0] != 0)
    {
        strcpy(g_grouplist[gid].user[u_id].username,account.nickname);
    }
    pnr_groupuser_dbinsert(gid,u_id,uindex,user_type,g_grouplist[gid].last_msgid,
        user_toxid,g_grouplist[gid].user[u_id].username,g_grouplist[gid].user[u_id].userkey);
    g_grouplist[gid].user_num++;
    pthread_mutex_unlock(&g_grouplock[gid]);
    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_adduser:gid(%d) adduser(%d:%s) type(%d)",gid,u_id,user_toxid,user_type);
    return OK;
}
/**********************************************************************************
  Function:      pnr_group_deluser
  Description: 删除群组成员
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int pnr_group_deluser(int gid,char* user_toxid)
{
    struct group_user* puser = NULL;
    if(gid < 0 || user_toxid == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_deluser:input params err");
        return ERROR;
    }
    pthread_mutex_lock(&g_grouplock[gid]);
    puser = get_guserbytoxid(gid,user_toxid);
    if(puser == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_deluser:user not in group");
        pthread_mutex_unlock(&g_grouplock[gid]);
        return ERROR;
    }
    if(puser->type == GROUP_USER_OWNER)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_deluser:gid(%d) user(%s) owner not del",gid,user_toxid);
        pthread_mutex_unlock(&g_grouplock[gid]);
        return ERROR;
    }
    //删除数据库记录
    pnr_groupuser_dbdelete_byuid(gid,puser->userindex);
    //删除内存记录
    memset(puser,0,sizeof(struct group_user));
    g_grouplist[gid].user_num--;
    pthread_mutex_unlock(&g_grouplock[gid]);
    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_deluser:gid(%d) deluser(%s)",gid,user_toxid);
    return OK;
}
/**********************************************************************************
  Function:      pnr_group_dissolve
  Description: 群解散
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int pnr_group_dissolve(int gid)
{
    char cmd[CMD_MAXLEN+1] = {0};
    if(gid < 0 || gid > PNR_GROUP_MAXNUM)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_group_dissolve:input params err");
        return ERROR;
    }
    pthread_mutex_lock(&g_grouplock[gid]);
    //删除数据库用户记录
    pnr_groupuser_dbdelete_byuid(gid,0);
    //删除数据库消息记录
    pnr_groupmsg_dbdelete_bymsgid(gid,0);
    //删除文件记录
    if(strlen(g_grouplist[gid].group_filepath) > strlen(DAEMON_PNR_USERDATA_DIR))
    {
        snprintf(cmd,CMD_MAXLEN,"rm -f %s/*",g_grouplist[gid].group_filepath);
        system(cmd);
    }
    //删除内存记录
    memset(&g_grouplist[gid],0,sizeof(struct group_info));    
    //删除群记录
    pnr_group_dbdelete_bygid(gid);
    g_grouplist[gid].init_flag = FALSE;
    g_groupnum--;
    pthread_mutex_unlock(&g_grouplock[gid]);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_group_dissolve:dissolve gid(%d)",gid);
    return OK;
}
/**********************************************************************************
  Function:      im_group_create_deal
  Description: IM模块创建群组消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_create_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = PNR_CREATEGROUP_RETCODE_OK;
    char* ret_buff = NULL;
    char user_toxid[TOX_ID_STR_LEN+1] = {0};
    char userkey[PNR_GROUP_USERKEY_MAXLEN+1] = {0};
    char groupname[PNR_USERNAME_MAXLEN+1] = {0};
    char tmpname[PNR_USERNAME_MAXLEN+1] = {0};
    char fidlist[PNR_GROUP_EXTINFO_MAXLEN+1] = {0};
    char fkeylist[PNR_GROUP_EXTINFO_MAXLEN+1] = {0};
    struct gpuser_maplist* puserlist = NULL;
    struct group_sys_msg group_sysmsg;
    int verify = -1;
    int uindex =0,tmplen = 0,i = 0,j = 0,gid = -1,to_uid = 0;
    if(params == NULL)
    {
        return ERROR;
    }
    puserlist = (struct gpuser_maplist*)malloc(sizeof(struct gpuser_maplist));
    if(puserlist == NULL)
    {
        return ERROR;
    }
    memset(puserlist,0,sizeof(struct gpuser_maplist));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GroupName",groupname,PNR_USERNAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserKey",userkey,PNR_GROUP_USERKEY_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"VerifyMode",verify, 0);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",fidlist,PNR_GROUP_EXTINFO_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendKey",fkeylist,PNR_GROUP_EXTINFO_MAXLEN);
    //uid检查
    uindex = get_indexbytoxid(user_toxid);
    if(uindex == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_create_deal:bad uid(%s)",user_toxid);
        ret_code = PNR_CREATEGROUP_RETCODE_BADUID;
    }
    else if(strlen(userkey) == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_create_deal:bad userkey");
        ret_code = PNR_CREATEGROUP_RETCODE_BADPARAMS;
    }
    else if(verify != TRUE && verify != FALSE)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_create_deal:bad verify");
        ret_code = PNR_CREATEGROUP_RETCODE_BADPARAMS;
    }
    else if(strlen(fidlist)>0 && im_group_usermap_analyze(fidlist,fkeylist,puserlist) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_create_deal:bad verify");
        ret_code = PNR_CREATEGROUP_RETCODE_BADPARAMS;
    }
    else
    {    
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        pthread_mutex_lock(&g_grouptoplock);
        if(g_groupnum >= PNR_GROUP_MAXNUM)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_create_deal:now group over");
            ret_code = PNR_CREATEGROUP_RETCODE_GROUPOVER;
        }
        else //新建一个群
        {
            for(i = 0;i<PNR_GROUP_MAXNUM;i++)
            {
                if(g_grouplist[i].init_flag == FALSE)
                {
                    gid = i;
                    break;
                }
            }
            if(gid >= PNR_GROUP_MAXNUM || gid < 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_create_deal:null group find err");
                ret_code = PNR_CREATEGROUP_RETCODE_GROUPOVER;
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"new group(%d)",gid);
                //生成一个模拟群组用户
                if(strlen(groupname) == 0)
                {
                    snprintf(tmpname,PNR_USERNAME_MAXLEN,"group%d_%d",gid,(int)time(NULL));
                    pnr_base64_encode(tmpname,strlen(tmpname),groupname,&tmplen);
                }
                //初始化群组
                pnr_group_create(gid,uindex,verify,groupname,user_toxid);
                //添加群主成员
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"group(%d) add owner",gid);
                if(pnr_group_adduser(gid,GROUP_USER_OWNER,user_toxid,userkey) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_create_deal:pnr_group_adduser err");
                    ret_code = PNR_CREATEGROUP_RETCODE_BADPARAMS;
                }
                else if(puserlist->usernum > 0)
                {
                    if(g_group_invitee_needack == TRUE)
                    {
                        //暂时不用确认
                    }
                    else//如果不需要用户同意，直接添加
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_INFO,"group(%d) add user",gid);
                        memset(&group_sysmsg,0,sizeof(group_sysmsg));
                        group_sysmsg.gid= gid;
                        strcpy(group_sysmsg.group_hid,g_grouplist[gid].group_hid);
                        group_sysmsg.from_uid = uindex;
                        strcpy(group_sysmsg.from_user,user_toxid);
                        group_sysmsg.type = GROUP_SYSMSG_NEWUSER;

                        for(i=0;i<puserlist->usernum;i++)
                        {
                            to_uid = get_indexbytoxid(puserlist->gpuser[i].userid);
                            if(to_uid)
                            {
                                if(pnr_group_adduser(gid,GROUP_USER_NORMAL,puserlist->gpuser[i].userid,puserlist->gpuser[i].userkey) != OK)
                                {
                                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_create_deal:pnr_group_adduser(%d:%s) failed",i,puserlist->gpuser[i].userid);
                                }
                                else
                                {
                                    strcpy(group_sysmsg.to_user,puserlist->gpuser[i].userid);
                                    group_sysmsg.to_uid = to_uid;
                                    pnr_groupoper_dbget_insert(gid,GROUP_OPER_ACTION_ADDUSER,uindex,i,g_grouplist[gid].group_name,user_toxid,puserlist->gpuser[i].userid,"create group in");
                                    pthread_mutex_lock(&g_grouplock[gid]);
                                    for(j=0;j<PNR_GROUP_USER_MAXNUM;j++)
                                    {
                                        if(g_grouplist[gid].user[j].userindex != 0 && g_grouplist[gid].user[j].userindex != uindex)
                                        {
                                            strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[j].toxid);
                                            im_pushmsg_callback(g_grouplist[gid].user[j].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                                        }
                                    }
                                    pthread_mutex_unlock(&g_grouplock[gid]);
                                }
                            }
                            else
                            {
                                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"failed to get user(%s)",puserlist->gpuser[i].userid);
                            }
                        }
                    }
                }
                g_groupnum++;
            }            
        }
        pthread_mutex_unlock(&g_grouptoplock);
    }
    free(puserlist);
    //构建响应消息
	cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_CREATEGROUP));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(user_toxid));
    cJSON_AddItemToObject(ret_params, "GAdmin", cJSON_CreateString(user_toxid));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(g_grouplist[gid].group_hid));
    cJSON_AddItemToObject(ret_params, "GName", cJSON_CreateString(groupname));
    cJSON_AddItemToObject(ret_params, "UserKey", cJSON_CreateString(userkey));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_group_invite_deal
  Description: IM模块邀请好友加入群组消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_invite_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char user_toxid[TOX_ID_STR_LEN+1] = {0};
    char group_hid[PNR_USERNAME_MAXLEN+1] = {0};
    char fidlist[PNR_GROUP_EXTINFO_MAXLEN+1] = {0};
    char fkeylist[PNR_GROUP_EXTINFO_MAXLEN+1] = {0};
    struct gpuser_maplist* puserlist = NULL;
    struct group_invite_info invite_info;
    struct group_user* pinviter = NULL;
    struct pnr_account_struct tmp_account;
    struct group_sys_msg group_sysmsg;
    int userindex =0,i = 0,j = 0,gid = 0,to_uid = 0;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    puserlist = (struct gpuser_maplist*)malloc(sizeof(struct gpuser_maplist));
    if(puserlist == NULL)
    {
        return ERROR;
    }
    memset(puserlist,0,sizeof(struct gpuser_maplist));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",user_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Gid",group_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",fidlist,PNR_GROUP_EXTINFO_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendKey",fkeylist,PNR_GROUP_EXTINFO_MAXLEN);
    //uid检查
    userindex = get_indexbytoxid(user_toxid);
    gid = get_gidbygrouphid(group_hid);
    pinviter = get_guserbyuindex(gid,userindex);
    if(userindex == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_invite_deal:bad uid(%s)",user_toxid);
        ret_code = PNR_GROUPINVITE_RETCODE_BADUID;
    }
    else if(gid < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_invite_deal:bad gid(%s)",group_hid);
        ret_code = PNR_GROUPINVITE_RETCODE_BADPARAMS;
    }
    else if(pinviter == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_invite_deal:bad user(%s) gid(%s) not found",user_toxid,group_hid);
        ret_code = PNR_GROUPINVITE_RETCODE_BADPARAMS;
    }
    else if(im_group_usermap_analyze(fidlist,fkeylist,puserlist) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_invite_deal:bad verify");
        ret_code = PNR_GROUPINVITE_RETCODE_BADPARAMS;
    }
    else
    {    
        if(*plws_index == 0)
        {
            *plws_index = userindex;
        }
        //如果需要管理员审批,并且自己不是管理员
        if(g_grouplist[gid].verify == TRUE && pinviter->type == GROUP_USER_NORMAL)
        {
            pthread_mutex_lock(&g_grouplock[gid]);
            memset(&invite_info,0,sizeof(invite_info));
            strcpy(invite_info.inviter,user_toxid);
            strcpy(invite_info.inviter_name,pinviter->username);
            strcpy(invite_info.group_name,g_grouplist[gid].group_name);
            strcpy(invite_info.aduitor,g_grouplist[gid].owner);
            strcpy(invite_info.groud_hid,g_grouplist[gid].group_hid);
            //向管理员推送待审核消息
            for(i=0;i<puserlist->usernum;i++)
            {
                memset(&tmp_account,0,sizeof(tmp_account));
                strcpy(tmp_account.toxid,puserlist->gpuser[i].userid);
                pnr_account_dbget_byuserid(&tmp_account);
                if(tmp_account.active == TRUE)
                {
                    strcpy(invite_info.user,puserlist->gpuser[i].userid);
                    strcpy(invite_info.user_groupkey,puserlist->gpuser[i].userkey);
                    strcpy(invite_info.user_name,tmp_account.nickname);
                    strcpy(invite_info.user_pubkey,tmp_account.user_pubkey);
                    im_pushmsg_callback(g_grouplist[gid].ownerid,PNR_IM_CMDTYPE_VERIFYGROUPPUSH,TRUE,head->api_version,&invite_info);
                }
                else
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get account bytid(%s) failed",puserlist->gpuser[i].userid);
                }
            }
            pthread_mutex_unlock(&g_grouplock[gid]);
        }
        else//否则直接添加入群了
        {
            memset(&group_sysmsg,0,sizeof(group_sysmsg));
            group_sysmsg.gid= gid;
            strcpy(group_sysmsg.group_hid,group_hid);
            group_sysmsg.from_uid = userindex;
            strcpy(group_sysmsg.from_user,user_toxid);
            group_sysmsg.type = GROUP_SYSMSG_NEWUSER;            
            for(i=0;i<puserlist->usernum;i++)
            {
                to_uid = get_indexbytoxid(puserlist->gpuser[i].userid);
                if(to_uid)
                {
                    group_sysmsg.to_uid = to_uid;
                    strcpy(group_sysmsg.to_user,puserlist->gpuser[i].userid);
                    if(pnr_group_adduser(gid,GROUP_USER_NORMAL,puserlist->gpuser[i].userid,puserlist->gpuser[i].userkey) != OK)
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_invite_deal:pnr_group_adduser(%d:%s) failed",i,puserlist->gpuser[i].userid);
                        ret_code = PNR_GROUPINVITE_RETCODE_GROUPOVER;
                    }
                    else
                    {
                        pnr_groupoper_dbget_insert(gid,GROUP_OPER_ACTION_ADDUSER,userindex,i,g_grouplist[gid].group_name,user_toxid,puserlist->gpuser[i].userid,"group invite in");
                        pthread_mutex_lock(&g_grouplock[gid]);
                        for(j=0;j<PNR_GROUP_USER_MAXNUM;j++)
                        {
                            if(g_grouplist[gid].user[j].userindex != 0 && g_grouplist[gid].user[j].userindex != userindex)
                            {
                                strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[j].toxid);
                                im_pushmsg_callback(g_grouplist[gid].user[j].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                            }
                        }
                        pthread_mutex_unlock(&g_grouplock[gid]);
                    }
                }
                else
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"failed to get user(%s)",puserlist->gpuser[i].userid);
                }
            }
        }
    }

    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_INVITEGROUP));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(user_toxid));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(g_grouplist[gid].group_hid));
    cJSON_AddItemToObject(ret_params, "GroupName", cJSON_CreateString(g_grouplist[gid].group_name));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_group_invitedeal_deal
  Description: IM模块邀请好友加入群组响应消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_invitedeal_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char user_toxid[TOX_ID_STR_LEN+1] = {0};
    int result = 0;
    char group_hid[TOX_ID_STR_LEN+1] = {0};
    char group_name[TOX_ID_STR_LEN+1] = {0};
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Userid",user_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"SelfId",user_toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",group_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GroupName",group_name,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Result",result,0);
   
    {
        //处理暂时不实现
        if(result != OK)
        {}
    }

    //构建响应消息
	cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPINVITEDEAL));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(user_toxid));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(""));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_group_verify_deal
  Description: IM模块审核好友加入群组消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_verify_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int result = 0,gid = 0,j = 0,uindex = 0,to_uid = 0;
    struct group_invite_info invite_info;
    struct group_sys_msg group_sysmsg;
    char group_name[PNR_USERNAME_MAXLEN+1] = {0};
    if(params == NULL)
    {
        return ERROR;
    }
    memset(&invite_info,0,sizeof(invite_info));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"From",invite_info.inviter,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"To",invite_info.user,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Aduit",invite_info.aduitor,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",invite_info.groud_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GName",group_name,PNR_USERNAME_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Result",result,0);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserKey",invite_info.user_groupkey,PNR_GROUP_USERKEY_MAXLEN);
    gid = get_gidbygrouphid(invite_info.groud_hid);
    uindex = get_indexbytoxid(invite_info.aduitor);
    if(gid < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_verify_deal:bad gid(%s)",invite_info.groud_hid);
        ret_code = PNR_GROUPVERIFY_RETCODE_BADGID;
    }
    else if(uindex == 0 || g_grouplist[gid].ownerid != uindex)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_verify_deal:bad uid(%s)",invite_info.aduitor);
        ret_code = PNR_GROUPVERIFY_RETCODE_BADUID;
    }
    else if(strlen(invite_info.inviter) == 0 || strlen(invite_info.user) == 0 || strlen(invite_info.user_groupkey) == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_verify_deal:bad params(%s:%s:%s)",invite_info.aduitor);
        ret_code = PNR_GROUPVERIFY_RETCODE_BADPARAMS;
    }
    if(result == OK)
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        memset(&group_sysmsg,0,sizeof(group_sysmsg));
        group_sysmsg.gid= gid;
        strcpy(group_sysmsg.group_hid,invite_info.groud_hid);
        group_sysmsg.from_uid = uindex;
        strcpy(group_sysmsg.from_user,invite_info.inviter);
        group_sysmsg.type = GROUP_SYSMSG_NEWUSER;  
        to_uid = get_indexbytoxid(invite_info.user);
        if(to_uid)
        {
            group_sysmsg.to_uid = to_uid;
            strcpy(group_sysmsg.to_user,invite_info.user);
            if(pnr_group_adduser(gid,GROUP_USER_NORMAL,invite_info.user,invite_info.user_groupkey))
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_verify_deal:bad params(%s:%s:%s)",invite_info.aduitor);
                ret_code = PNR_GROUPVERIFY_RETCODE_BADPARAMS;
            }
            else
            {
                pnr_groupoper_dbget_insert(gid,GROUP_OPER_ACTION_ADDUSER,uindex,uindex,g_grouplist[gid].group_name,invite_info.inviter,invite_info.user,"group verify in");
                pthread_mutex_lock(&g_grouplock[gid]);
                for(j=0;j<PNR_GROUP_USER_MAXNUM;j++)
                {
                    if(g_grouplist[gid].user[j].userindex != 0 && g_grouplist[gid].user[j].userindex != uindex)
                    {
                        strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[j].toxid);
                        im_pushmsg_callback(g_grouplist[gid].user[j].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                    }
                }
                pthread_mutex_unlock(&g_grouplock[gid]);
            }
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_verify_deal:fail to get user(%s)",invite_info.user);
        }
    }
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_VERIFYGROUP));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(invite_info.aduitor));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(""));
    cJSON_AddItemToObject(ret_params, "GAdmin", cJSON_CreateString(invite_info.aduitor));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(invite_info.groud_hid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_group_quit_deal
  Description: IM模块退出群组消息解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_quit_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char userid[TOX_ID_STR_LEN+1] = {0};
    char group_hid[TOX_ID_STR_LEN+1] = {0};
    char gname[PNR_USERNAME_MAXLEN+1] = {0};
    int gid = 0,uindex= 0,i = 0;
    struct group_sys_msg group_sysmsg;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",userid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",group_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GroupName",gname,PNR_USERNAME_MAXLEN);

    //参数检查
    uindex = get_indexbytoxid(userid);
    gid = get_gidbygrouphid(group_hid);
    if(uindex == 0 || gid < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_quit_deal:bad params(%s:%s)",userid,group_hid);
        ret_code = PNR_GROUPQUIT_RETCODE_BADPARAMS;
    }
    else 
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        memset(&group_sysmsg,0,sizeof(group_sysmsg));
        group_sysmsg.gid= gid;
        strcpy(group_sysmsg.group_hid,group_hid);
        group_sysmsg.from_uid = uindex;
        strcpy(group_sysmsg.from_user,userid);
        //如果是群主退出，就直接解散群
        if(uindex == g_grouplist[gid].ownerid)
        {
            group_sysmsg.type = GROUP_SYSMSG_DISGROUP;
            strcpy(group_sysmsg.msgpay,g_grouplist[gid].group_name);
            pthread_mutex_lock(&g_grouplock[gid]);
            for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
            {
                if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != uindex)
                {
                    group_sysmsg.to_uid = g_grouplist[gid].user[i].userindex;
                    strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[i].toxid);
                    im_pushmsg_callback(g_grouplist[gid].user[i].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                }
            }
            pthread_mutex_unlock(&g_grouplock[gid]);
            pnr_groupoper_dbget_insert(gid,GROUP_OPER_ACTION_DISOLVE,uindex,uindex,g_grouplist[gid].group_name,userid,userid,"dissolve group");
            pnr_group_dissolve(gid);
        }
        else
        {
            group_sysmsg.type = GROUP_SYSMSG_SELFOUT;
            pthread_mutex_lock(&g_grouplock[gid]);
            for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
            {
                if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != uindex)
                {
                    group_sysmsg.to_uid = g_grouplist[gid].user[i].userindex;
                    strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[i].toxid);
                    im_pushmsg_callback(g_grouplist[gid].user[i].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                }
            }
            pthread_mutex_unlock(&g_grouplock[gid]);
            pnr_group_deluser(gid,userid);
            pnr_groupoper_dbget_insert(gid,GROUP_OPER_ACTION_DELUSER,uindex,uindex,g_grouplist[gid].group_name,userid,userid,"group self quit");
        }
    }
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPQUIT));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(userid));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(group_hid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}


/**********************************************************************************
  Function:      im_group_pulllist_deal
  Description: IM模块拉取自己的群组列表解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_pulllist_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = PNR_GROUPPULL_RETCODE_OK;
    char* ret_buff = NULL;
    char userid[TOX_ID_STR_LEN+1] = {0};
    char rid[TOX_ID_STR_LEN+1] = {0};
    char startid[TOX_ID_STR_LEN+1] = {0};
    int i = 0,num = 0,gid = -1,tmp_gid = -1;
    char **dbResult; 
    char *errmsg;
    int nRow, nColumn;
    char sql_cmd[SQL_CMD_LEN] = {0};
    int offset=0,uindex= 0;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",userid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",rid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"TargetNum",num,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"StartId",startid,TOX_ID_STR_LEN);

    //参数检查
    uindex = get_indexbytoxid(userid);
    if(uindex < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_pulllist_deal:bad uid(%s)",userid);
        ret_code = PNR_GROUPPULL_RETCODE_ERR;
    }
    else if(num > 0)
    {
        gid = get_gidbygrouphid(startid);
        if(gid < 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_pulllist_deal:bad startid(%s)",startid);
            ret_code = PNR_GROUPPULL_RETCODE_ERR;
        }
    }
    
    cJSON *ret_root = cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPLISTPULL));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(userid));
    if(ret_code == PNR_GROUPPULL_RETCODE_OK)
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        if(num == 0)
        {
            //groupuser_tbl(gid,uid,uindex,type,initmsgid,lastmsgid,timestamp,utoxid,uname,uremark,gremark,pubkey)
            snprintf(sql_cmd, SQL_CMD_LEN, "select gid,gremark,pubkey from groupuser_tbl where uindex=%d;",uindex);
        }
        else
        {
            snprintf(sql_cmd, SQL_CMD_LEN, "select gid,gremark,pubkey from groupuser_tbl where uindex=%d and gid > %d order by gid desc limit %d;",
                uindex,gid,num);
        }
        if(sqlite3_get_table(g_groupdb_handle, sql_cmd, &dbResult, &nRow, 
            &nColumn, &errmsg) == SQLITE_OK)
        {
            offset = nColumn; //字段值从offset开始呀
            for( i = 0; i < nRow ; i++ )
            {
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    return ERROR;
                }
                cJSON_AddItemToArray(pJsonArry,pJsonsub);
                tmp_gid = atoi(dbResult[offset]);
                if(tmp_gid >=0 && tmp_gid < PNR_GROUP_MAXNUM)
                {
                    cJSON_AddStringToObject(pJsonsub,"GId",g_grouplist[tmp_gid].group_hid);
                    cJSON_AddNumberToObject(pJsonsub,"Verify",g_grouplist[tmp_gid].verify);
                    cJSON_AddStringToObject(pJsonsub,"GName",g_grouplist[tmp_gid].group_name);
                    cJSON_AddStringToObject(pJsonsub,"GAdmin",g_grouplist[tmp_gid].owner);
                    if(dbResult[offset+1] != NULL)
                    {
                        cJSON_AddStringToObject(pJsonsub,"Remark",dbResult[offset+1]);
                    }
                    else
                    {
                        cJSON_AddStringToObject(pJsonsub,"Remark","");
                    }
                    if(dbResult[offset+2] != NULL)
                    {
                        cJSON_AddStringToObject(pJsonsub,"UserKey",dbResult[offset+2]);
                    }
                    else
                    {
                        cJSON_AddStringToObject(pJsonsub,"UserKey","");
                    }
                }
                offset += nColumn;
            }
            sqlite3_free_table(dbResult);
        }
        cJSON_AddItemToObject(ret_params, "GroupNum", cJSON_CreateNumber(nRow));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_group_pulluser_deal
  Description: IM模块拉取自己的群组用户列表解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_pulluser_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = PNR_GROUPPULL_RETCODE_OK;
    char* ret_buff = NULL;
    char userid[TOX_ID_STR_LEN+1] = {0};
    char rid[TOX_ID_STR_LEN+1] = {0};
    char group_hid[TOX_ID_STR_LEN+1] = {0};
    char startid[TOX_ID_STR_LEN+1] = {0};
    int i = 0,num = 0,gid = -1,tmp_uid = -1,uid = 0,fid = -1;
    char **dbResult; 
    char *errmsg;
    int nRow, nColumn;
    char sql_cmd[SQL_CMD_LEN] = {0};
    int offset=0,uindex= 0;
    struct group_user* pgusr = NULL;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",userid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",rid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",group_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"TargetNum",num,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"StartId",startid,TOX_ID_STR_LEN);

    //参数检查
    uindex = get_indexbytoxid(userid);
    gid = get_gidbygrouphid(group_hid);
    if(uindex < 0 || gid < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_pulluser_deal:bad uid(%s) gid(%s)",userid,group_hid);
        ret_code = PNR_GROUPPULL_RETCODE_ERR;
    }
    else if(num > 0 && strlen(startid) == TOX_ID_STR_LEN)
    {
        uid = get_indexbytoxid(startid);
        if(uid <= 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_pulluser_deal:bad startid(%s)",startid);
            ret_code = PNR_GROUPPULL_RETCODE_ERR;
        }
    }
    cJSON *ret_root = cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPUSERPULL));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(userid));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(group_hid));
    cJSON_AddItemToObject(ret_params, "Verify", cJSON_CreateNumber(g_grouplist[gid].verify));
    if(ret_code == PNR_GROUPPULL_RETCODE_OK)
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        if(num == 0)
        {
            //groupuser_tbl(gid,uid,uindex,type,initmsgid,lastmsgid,timestamp,utoxid,uname,uremark,gremark,pubkey)
            snprintf(sql_cmd, SQL_CMD_LEN, "select uindex,type,utoxid,uname,uremark from groupuser_tbl where gid=%d;",gid);
        }
        else
        {
            snprintf(sql_cmd, SQL_CMD_LEN, "select uindex,type,utoxid,uname,uremark from groupuser_tbl where gid=%d order by uindex desc limit %d;",gid,num);
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_group_pulluser_deal:sql_cmd(%s)",sql_cmd);
        if(sqlite3_get_table(g_groupdb_handle, sql_cmd, &dbResult, &nRow, 
            &nColumn, &errmsg) == SQLITE_OK)
        {
            offset = nColumn; //字段值从offset开始呀
            for( i = 0; i < nRow ; i++ )
            {
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    return ERROR;
                }
                cJSON_AddItemToArray(pJsonArry,pJsonsub);
                tmp_uid = atoi(dbResult[offset]);
                cJSON_AddNumberToObject(pJsonsub,"Id",tmp_uid);
                cJSON_AddNumberToObject(pJsonsub,"Type",atoi(dbResult[offset+1]));
                cJSON_AddNumberToObject(pJsonsub,"Local",TRUE);
                if(dbResult[offset+2] != NULL)
                {
                    cJSON_AddStringToObject(pJsonsub,"ToxId",dbResult[offset+2]);
                }
                else
                {
                    cJSON_AddStringToObject(pJsonsub,"ToxId","");
                }
                if(dbResult[offset+3] != NULL)
                {
                    cJSON_AddStringToObject(pJsonsub,"Nickname",dbResult[offset+3]);
                }
                else
                {
                    cJSON_AddStringToObject(pJsonsub,"Nickname","");
                }
                if(dbResult[offset+4] != NULL)
                {
                    cJSON_AddStringToObject(pJsonsub,"Remarks",dbResult[offset+4]);
                }
                else
                {
                    //这里如果群昵称里面没有设置，再检测是不是自己的好友，如果是自己好友并且好友昵称里面设置了，返回好友昵称
                    fid = get_friendid_bytoxid(uindex,dbResult[offset+2]);
                    if(fid >= 0)
                    {
                        if(strlen(g_imusr_array.usrnode[uindex].friends[fid].user_remarks) > 0)
                        {
                            cJSON_AddStringToObject(pJsonsub,"Remarks", g_imusr_array.usrnode[uindex].friends[fid].user_remarks);
                        }
                        else
                        {
                            cJSON_AddStringToObject(pJsonsub,"Remarks","");
                        }
                    }
                    else
                    {
                        cJSON_AddStringToObject(pJsonsub,"Remarks","");
                    }
                }
                pthread_mutex_lock(&g_grouplock[gid]);
                pgusr = get_guserbyuindex(gid,tmp_uid);
                if(pgusr)
                {
                    cJSON_AddStringToObject(pJsonsub,"UserKey",pgusr->user_pubkey);
                }
                pthread_mutex_unlock(&g_grouplock[gid]);
                offset += nColumn;
            }
            sqlite3_free_table(dbResult);
        }
        cJSON_AddItemToObject(ret_params, "UserNum", cJSON_CreateNumber(nRow));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_group_pullmsg_deal
  Description: IM模块拉取自己的群消息列表解析处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_pullmsg_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = PNR_GROUPPULL_RETCODE_OK;
    char* ret_buff = NULL;
    char userid[TOX_ID_STR_LEN+1] = {0};
    char rid[TOX_ID_STR_LEN+1] = {0};
    char group_hid[TOX_ID_STR_LEN+1] = {0};
    int i = 0,num = 0,gid = -1,tmp_msgid = -1,msgtype = 0,tmpmsgtype = 0,tmp_uid = 0;
    int msg_startid = 0,new_lastmsgid = 0;
    int msg_initid = 0;
    struct group_fileinfo_struct tmp_finfo;
    struct pnr_account_struct tmpaccount;
    char **dbResult; 
    char *errmsg;
    int nRow, nColumn = 0;
    char sql_cmd[SQL_CMD_LEN] = {0};
    char sql_cmd_cat[PNR_FILENAME_MAXLEN+1] = {0};
    int offset=0,uindex= 0,usertype = GROUP_USER_NORMAL;
    struct group_user* p_guser= NULL;
    struct group_user* p_gowner= NULL;
    char* pattend = NULL;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",userid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",rid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",group_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgType",msgtype,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgNum",num,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgStartId",msg_startid,TOX_ID_STR_LEN);

    //参数检查
    uindex = get_indexbytoxid(userid);
    gid = get_gidbygrouphid(group_hid);
    if(uindex < 0 || gid < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_pullmsg_deal:bad uid(%s) gid(%s)",userid,group_hid);
        ret_code = PNR_GROUPPULL_RETCODE_ERR;
    }
    else
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        p_gowner = get_guserbyuindex(gid,uindex);
        if(p_gowner == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_pullmsg_deal:bad uid(%s) gid(%s)",userid,group_hid);
            ret_code = PNR_GROUPPULL_RETCODE_ERR;
        }
        else
        {
            msg_initid = p_gowner->init_msgid;
            usertype = p_gowner->type;
        }
    }
    cJSON *ret_root = cJSON_CreateObject();
    if(ret_root == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        return ERROR;
    }
    cJSON *ret_params = cJSON_CreateObject();
    cJSON *pJsonArry = cJSON_CreateArray();
    cJSON *pJsonsub = NULL;
    if(pJsonArry == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPMSGPULL));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(userid));
    cJSON_AddItemToObject(ret_params, "UserType", cJSON_CreateNumber(usertype));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(group_hid));
    if(ret_code == PNR_GROUPPULL_RETCODE_OK)
    {
        switch(msgtype)
        {
            case PNR_GROUPPULL_MSGTYPE_ALL:
                //groupmsg_tbl(gid,msgid,userindex,timestamp,msgtype,sender,msg,attend,ext,ext2,filekey)
                snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select msgid,msgtype,userindex,timestamp,sender,msg,attend,ext,ext2,filekey from groupmsg_tbl where gid=%d and msgid > %d",gid,msg_initid);
                break;
            case PNR_GROUPPULL_MSGTYPE_TEXT:
                //groupmsg_tbl(gid,msgid,userindex,timestamp,msgtype,sender,msg,attend,ext,ext2,filekey)
                snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select msgid,msgtype,userindex,timestamp,sender,msg,attend,ext,ext2,filekey from groupmsg_tbl where gid=%d and msgid > %d and msgtype=%d",gid,msg_initid,PNR_IM_MSGTYPE_TEXT);
                break;
            case PNR_GROUPPULL_MSGTYPE_FILE:
                //groupmsg_tbl(gid,msgid,userindex,timestamp,msgtype,sender,msg,attend,ext,ext2,filekey)
                snprintf(sql_cmd, SQL_CMD_LEN, "select * from(select msgid,msgtype,userindex,timestamp,sender,msg,attend,ext,ext2,filekey from groupmsg_tbl where gid=%d and msgid > %d and msgtype IN(%d,%d,%d,%d)",
                gid,msg_initid,PNR_IM_MSGTYPE_IMAGE,PNR_IM_MSGTYPE_AUDIO,
                PNR_IM_MSGTYPE_MEDIA,PNR_IM_MSGTYPE_FILE);
                break;
        }
        if(num != 0)
        {
            if(msg_startid != 0)
            {
                snprintf(sql_cmd_cat,PNR_BUF_LEN_128," and msgid < %d order by msgid desc limit %d)temp order by msgid;",msg_startid,num);
            }
            else
            {
                snprintf(sql_cmd_cat,PNR_BUF_LEN_128," order by msgid desc limit %d)temp order by msgid;",num);
            }
            strcat(sql_cmd,sql_cmd_cat);
        }
        else
        {
            strcat(sql_cmd,")temp order by msgid;");            
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_group_pullmsg_deal:sqlcmd(%s)",sql_cmd);
        if(sqlite3_get_table(g_groupdb_handle, sql_cmd, &dbResult, &nRow, 
            &nColumn, &errmsg) == SQLITE_OK)
        {
            offset = nColumn; //字段值从offset开始呀
            for( i = 0; i < nRow ; i++ )
            {
                pJsonsub = cJSON_CreateObject();
                if(pJsonsub == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                    cJSON_Delete(ret_root);
                    return ERROR;
                }
                cJSON_AddItemToArray(pJsonArry,pJsonsub);
                tmp_msgid = atoi(dbResult[offset]);
                if(i == 0)
                {
                    new_lastmsgid = tmp_msgid;
                }
                tmpmsgtype = atoi(dbResult[offset+1]);
                tmp_uid = atoi(dbResult[offset+2]);
                cJSON_AddNumberToObject(pJsonsub,"MsgId",tmp_msgid);
                cJSON_AddNumberToObject(pJsonsub,"MsgType",tmpmsgtype);
                cJSON_AddNumberToObject(pJsonsub,"TimeStamp",atoi(dbResult[offset+3]));
                //自己发的消息，不用from
                if(tmp_uid == uindex)
                {
                    cJSON_AddStringToObject(pJsonsub,"From","");
                }
                else if(dbResult[offset+4] != NULL)
                {
                    cJSON_AddStringToObject(pJsonsub,"From",dbResult[offset+4]);
                }
                if(dbResult[offset+5] != NULL)
                {
                    if(tmpmsgtype == PNR_IM_MSGTYPE_IMAGE || tmpmsgtype == PNR_IM_MSGTYPE_AUDIO
                          || tmpmsgtype == PNR_IM_MSGTYPE_MEDIA || tmpmsgtype == PNR_IM_MSGTYPE_FILE)
                    {
                        cJSON_AddStringToObject(pJsonsub,"FileName",dbResult[offset+5]);
                        cJSON_AddStringToObject(pJsonsub,"Msg","");
                    }
                    else
                    {
                        cJSON_AddStringToObject(pJsonsub,"FileName","");
                        cJSON_AddStringToObject(pJsonsub,"Msg",dbResult[offset+5]);
                    }
                }
                //只有未拉取过的消息，才需要判断point位
                if(p_gowner != NULL && tmp_msgid > p_gowner->last_msgid)
                {
                    //attend项
                    if(dbResult[offset+6] != NULL)
                    {
                        pattend = dbResult[offset+6];
                        if((strstr(pattend,"all") != NULL)||(strstr(pattend,"ALL") != NULL))
                        {
                            cJSON_AddNumberToObject(pJsonsub,"Point",PNR_GROUP_MSGPUSH_ATTEND_ALL);
                        }
                        else if(pnr_stristr(pattend,userid) != NULL)
                        {
                            cJSON_AddNumberToObject(pJsonsub,"Point",PNR_GROUP_MSGPUSH_ATTEND_ONLY);
                        }
                        else
                        {
                            cJSON_AddNumberToObject(pJsonsub,"Point",PNR_GROUP_MSGPUSH_ATTEND_NONE);
                        }
                    }
                }
                if(tmpmsgtype == PNR_IM_MSGTYPE_IMAGE || tmpmsgtype == PNR_IM_MSGTYPE_AUDIO
                    || tmpmsgtype == PNR_IM_MSGTYPE_MEDIA || tmpmsgtype == PNR_IM_MSGTYPE_FILE)
                {
                    //ext 存储文件相对路径
                    if(dbResult[offset+7] != NULL)
                    {
                        cJSON_AddStringToObject(pJsonsub,"FilePath",dbResult[offset+7]);
                    }
                    //ext2 存储文件相对相关属性
                    if(dbResult[offset+8] != NULL)
                    {
                        memset(&tmp_finfo,0,sizeof(tmp_finfo));
                        if(im_group_fileinfo_analyze(dbResult[offset+8],&tmp_finfo) == OK)
                        {
                            cJSON_AddStringToObject(pJsonsub,"FileMD5",tmp_finfo.md5);
                            cJSON_AddNumberToObject(pJsonsub,"FileSize",tmp_finfo.filesize);
                            if(strlen(tmp_finfo.attach_info) > 0) 
                            {
                                cJSON_AddStringToObject(pJsonsub,"FileInfo",tmp_finfo.attach_info);
                            }
                        }
                    }
                    //filekey 只有转发的文件才有
                    if(dbResult[offset+9] != NULL)
                    {
                        cJSON_AddStringToObject(pJsonsub,"FileKey",dbResult[offset+9]);
                    }
                }
                pthread_mutex_lock(&g_grouplock[gid]);
                p_guser = get_guserbyuindex(gid,tmp_uid);
                if(p_guser)
                {
                    cJSON_AddStringToObject(pJsonsub,"UserName",p_guser->username);
                    cJSON_AddStringToObject(pJsonsub,"UserKey",p_guser->user_pubkey);
                }
                else
                {
                    //这里可能用户已经退群了，所以要用toxid去找对应的账户
                    memset(&tmpaccount,0,sizeof(tmpaccount));
                    if(dbResult[offset+4])
                    {
                        strcpy(tmpaccount.toxid,dbResult[offset+4]);
                        pnr_account_dbget_byuserid(&tmpaccount);
                        if(tmpaccount.active == TRUE)
                        {
                            cJSON_AddStringToObject(pJsonsub,"UserName",tmpaccount.nickname);
                            cJSON_AddStringToObject(pJsonsub,"UserKey",tmpaccount.user_pubkey);
                        }
                    }
                }
                pthread_mutex_unlock(&g_grouplock[gid]);
                
                offset += nColumn;
            }
            sqlite3_free_table(dbResult);
        }
        cJSON_AddItemToObject(ret_params, "MsgNum", cJSON_CreateNumber(nRow));
        cJSON_AddItemToObject(ret_params,"Payload", pJsonArry);
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    //更新拉取到的lastmsgid
    if(p_gowner != NULL && new_lastmsgid > p_gowner->last_msgid)
    {
        p_gowner->last_msgid = new_lastmsgid;
        pnr_groupuser_lastmsgid_dbupdate_byid(p_gowner->gindex,p_gowner->userindex,new_lastmsgid);
    }
    return OK;
}

/**********************************************************************************
  Function:      im_group_sendmsg_deal
  Description: IM模块用户群里面发消息
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_sendmsg_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    struct group_user_msg* pmsg = NULL;
    struct group_user* puser = NULL;
    char group_hid[TOX_ID_STR_LEN+1] = {0};
    int uindex= 0,gid = -1,i=0;
    if(params == NULL)
    {
        return ERROR;
    }
    pmsg = (struct group_user_msg*)malloc(sizeof(struct group_user_msg));
    if(pmsg == NULL)
    {
        return ERROR;
    }
    memset(pmsg,0,sizeof(struct group_user_msg));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",pmsg->from,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",group_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Point",pmsg->attend,PNR_GROUP_EXTINFO_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Msg",pmsg->msgpay,PNR_GROUP_USERMSG_MAXLEN);

    //参数检查
    uindex = get_indexbytoxid(pmsg->from);
    gid = get_gidbygrouphid(group_hid);
    if(uindex == 0 || group_hid < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_sendmsg_deal:bad uid(%s) gid(%d)",pmsg->from,group_hid);
        ret_code = PNR_GROUPSENDMSG_RETCODE_BADPARAMS;
    }
    else if (pnr_checkrepeat_bymsgid(uindex, head->msgid,&(head->repeat_flag)) == TRUE) 
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
		ret_code = PNR_GROUPSENDMSG_RETCODE_OK;
	}
    else 
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        pmsg->gid = gid;
        pmsg->from_uid = uindex;
        pmsg->type = PNR_IM_MSGTYPE_TEXT;
        pmsg->timestamp = (int)time(NULL);
        puser = get_guserbyuindex(gid,uindex);
        if(puser == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%d)",pmsg->from);
            ret_code = PNR_GROUPSENDMSG_RETCODE_BADUSER;
        }
        else
        {
            if((strstr(pmsg->attend,"all") != NULL)||(strstr(pmsg->attend,"ALL") != NULL))
            {
                pmsg->attend_all = TRUE;
            }
			ret_code = PNR_GROUPSENDMSG_RETCODE_OK;
            pthread_mutex_lock(&g_grouplock[gid]);
            g_grouplist[gid].last_msgid++;
            pmsg->msgid = g_grouplist[gid].last_msgid;
            strcpy(pmsg->group_name,g_grouplist[gid].group_name);
            pnr_groupmsg_dbinsert(gid,uindex,pmsg->msgid,PNR_IM_MSGTYPE_TEXT,
                pmsg->from,pmsg->msgpay,pmsg->attend,NULL,NULL,NULL);
            for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
            {
                if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != uindex)
                {
                    pmsg->to_uid = g_grouplist[gid].user[i].userindex;
                    strcpy(pmsg->to,g_grouplist[gid].user[i].toxid);
                    strcpy(pmsg->to_key,g_grouplist[gid].user[i].userkey);
                    if(strlen(g_grouplist[gid].user[i].group_remarks) > 0)
                    {
                        memset(pmsg->group_name,0,PNR_GROUP_USERKEY_MAXLEN);
                        strcpy(pmsg->group_name,g_grouplist[gid].user[i].group_remarks);
                    }
                    im_pushmsg_callback(pmsg->to_uid,PNR_IM_CMDTYPE_GROUPMSGPUSH,TRUE,head->api_version,(void *)pmsg);
                }
            }
            pthread_mutex_unlock(&g_grouplock[gid]);
        }
    }

    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        free(pmsg);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPSENDMSG));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(pmsg->from));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(pmsg->msgid));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(group_hid));
    if(puser != NULL && strlen(puser->group_remarks) > 0)
    {
        cJSON_AddItemToObject(ret_params, "Gname", cJSON_CreateString(puser->group_remarks));
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "Gname", cJSON_CreateString(g_grouplist[gid].group_name));
    }
    if(puser)
    {
        cJSON_AddItemToObject(ret_params, "UserKey", cJSON_CreateString(puser->userkey));
    }
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(pmsg->msgpay));
    if(head->repeat_flag == TRUE)
    {
        cJSON_AddItemToObject(ret_params, "Repeat", cJSON_CreateNumber(TRUE));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    free(pmsg);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_group_delmsg_deal
  Description: IM模块用户群里面删除消息
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_delmsg_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    struct group_user_msg* pmsg = NULL;
    struct group_user* puser = NULL;
    struct group_sys_msg group_sysmsg;
    char group_hid[TOX_ID_STR_LEN+1] = {0};
    char userid[TOX_ID_STR_LEN+1] = {0};
    char filepathcmd[PNR_FILEPATH_MAXLEN+1] = {0};
    int uindex= 0,gid = -1,i=0,msgid = 0;
    int del_type = 0;
    if(params == NULL)
    {
        return ERROR;
    }
    pmsg = (struct group_user_msg*)malloc(sizeof(struct group_user_msg));
    if(pmsg == NULL)
    {
        return ERROR;
    }
    memset(pmsg,0,sizeof(struct group_user_msg));
    //解析参数
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Type",del_type,0);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"From",userid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",group_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgId",msgid,0);

    //参数检查
    uindex = get_indexbytoxid(userid);
    gid = get_gidbygrouphid(group_hid);
    if(uindex == 0 || group_hid < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_delmsg_deal:bad uid(%s) gid(%s)",userid,group_hid);
        ret_code = PNR_GROUP_DELMSG_RETURN_BADPRARMS;
    }
    else 
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        pnr_groupmsg_dbget_bymsgid(gid,msgid,pmsg);
        puser = get_guserbyuindex(gid,uindex);
        if(puser == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_delmsg_deal:gid(%d) uid(%d) not finde",gid,uindex);
            ret_code = PNR_GROUP_DELMSG_RETURN_BADPRARMS;
        }
        else if((del_type == PNR_GROUP_DELMSG_TYPE_ADMIN)&&(puser->type == GROUP_USER_NORMAL))
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_delmsg_deal:deltype(%d) but usertype(%d)",del_type,puser->type);
            ret_code = PNR_GROUP_DELMSG_RETURN_NOPOWER;
        }
        else if((del_type == PNR_GROUP_DELMSG_TYPE_SELF)&&(puser->userindex != pmsg->from_uid))
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_delmsg_deal:deltype(%d) uindex(%d) but sendid(%d)",del_type,puser->userindex,pmsg->from_uid);
            ret_code = PNR_GROUP_DELMSG_RETURN_NOPOWER;
        }
        else
        {
			ret_code = PNR_GROUP_DELMSG_RETURN_OK;
            memset(&group_sysmsg,0,sizeof(group_sysmsg));
            if(del_type == PNR_GROUP_DELMSG_TYPE_SELF)
            {
                group_sysmsg.type = GROUP_SYSMSG_DELMSG_BYSELF;
            }
            else
            {
                group_sysmsg.type = GROUP_SYSMSG_DELMSG_BYADMIN;
            }
            strcpy(group_sysmsg.from_user,userid);
            group_sysmsg.msgid = msgid;
            group_sysmsg.from_uid = uindex;
            group_sysmsg.gid = gid;
            strcpy(group_sysmsg.group_hid,group_hid);
            pthread_mutex_lock(&g_grouplock[gid]);
            //删除数据库记录
            pnr_groupmsg_dbdelete_bymsgid(gid,msgid);
            //如果是文件，删除文件
            if(pmsg->type == PNR_IM_MSGTYPE_IMAGE || pmsg->type == PNR_IM_MSGTYPE_AUDIO 
                || pmsg->type == PNR_IM_MSGTYPE_MEDIA || pmsg->type == PNR_IM_MSGTYPE_FILE)
            {
                if(strlen(pmsg->ext1) > 0 && strstr(pmsg->ext1,PNR_GROUP_DATA_PATH) != NULL)
                {
                    strcpy(filepathcmd,"rm -f ");
                    strcat(filepathcmd,WS_SERVER_INDEX_FILEPATH);
                    strcat(filepathcmd,pmsg->ext1);
                    system(filepathcmd);
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_group_delmsg_deal:del file(%s)",filepathcmd);
                }               
            }
            for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
            {
                if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != uindex)
                {
                    group_sysmsg.to_uid = g_grouplist[gid].user[i].userindex;
                    strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[i].toxid);
                    im_pushmsg_callback(g_grouplist[gid].user[i].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                }
            }
            pthread_mutex_unlock(&g_grouplock[gid]);

        }
    }

    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        free(pmsg);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPDELMSG));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msgid));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(userid));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(group_hid));
    cJSON_AddItemToObject(ret_params, "Msg", cJSON_CreateString(""));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    free(pmsg);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_group_sendfile_predeal
  Description: IM模块用户群里面发文件预处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
暂时不用
***********************************************************************************/
int im_group_sendfile_predeal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char router_id[TOX_ID_STR_LEN+1] = {0};
    struct pnr_account_struct account;
    struct pnr_account_struct src_account;
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    int datafile_version = 0;
    if(params == NULL)
    {
        return ERROR;
    }

    //解析参数
    memset(&account,0,sizeof(account));
    memset(&src_account,0,sizeof(src_account));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouteId",router_id,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserSn",account.user_sn,PNR_USN_MAXLEN);
    if(head->api_version == PNR_API_VERSION_V4)
    {
        CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Pubkey",src_account.user_pubkey,PNR_USER_PUBKEY_MAXLEN);
    }

    //rid检查
    if(strncmp(router_id,g_daemon_tox.user_toxid,TOX_ID_STR_LEN) != OK)
    {
        ret_code = PNR_RECOVERY_RETCODE_BAD_RID;
    }
    else if(head->api_version == PNR_API_VERSION_V4)
    {
        if(strlen(src_account.user_pubkey) > 0)
        {
            pnr_account_dbget_byuserkey(&src_account);
            if(src_account.active == FALSE)
            {
                //该用户未注册激活过 
                //临时用户直接返回
                if(strcmp(account.user_sn,g_account_array.temp_user_sn) == OK)
                {
                    ret_code = PNR_RECOVERY_RETCODE_TEMP_USER;
                }
                else
                {
                    //查看当前二维码账户是否已经被占用
                    pnr_account_get_byusn(&account);
                    if(account.active == FALSE)
                    {
                        ret_code = PNR_RECOVERY_RETCODE_USER_NOACTIVE;
                    }
                    else
                    {
                        ret_code = PNR_RECOVERY_RETCODE_QRCODE_USED;
                    }
                }
            }
            else//该用户pubkey已经注册过
            {
                ret_code = PNR_RECOVERY_RETCODE_USER_ACTIVED;
                if(strcmp(account.user_sn,src_account.user_sn) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_user_recovery_deal:input sn(%s) and regiseted sn(%s)",
                        account.user_sn,src_account.user_sn);
                }
                memcpy(&account,&src_account,sizeof(account));
            }
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_user_recovery_deal:input pubkey null");
            ret_code = PNR_RECOVERY_RETCODE_OTHERS_ERROR;
        }
    }
    else
    {    
        if(strcmp(account.user_sn,g_account_array.temp_user_sn) == OK)
        {
            ret_code = PNR_RECOVERY_RETCODE_TEMP_USER;
        }
        else
        {
            //根据usn获取当前数据库中的账号信息
            pnr_account_get_byusn(&account);
            //非临时账户只有已激活的账户才可以找回
            if(account.type < PNR_USER_TYPE_ADMIN || account.type >= PNR_USER_TYPE_BUTT)
            {
                ret_code = PNR_RECOVERY_RETCODE_OTHERS_ERROR;
            }
            else if(account.active == TRUE)
            {
                ret_code = PNR_RECOVERY_RETCODE_USER_ACTIVED;
            }
            else
            {
                ret_code = PNR_RECOVERY_RETCODE_USER_NOACTIVE;
            }        
        }
    }

    //构建响应消息
	cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_RECOVERY));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "RouteId", cJSON_CreateString(g_daemon_tox.user_toxid));
    if(strcmp(g_dev_nickname,DB_DEFAULT_DEVNAME_VALUE) != OK)
    {
        cJSON_AddItemToObject(ret_params, "RouterName", cJSON_CreateString(g_dev_nickname));
    }
    cJSON_AddItemToObject(ret_params, "UserSn", cJSON_CreateString(account.user_sn));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(account.toxid));
    cJSON_AddItemToObject(ret_params, "NickName", cJSON_CreateString(account.nickname));
    cJSON_AddItemToObject(ret_params, "DataFileVersion", cJSON_CreateNumber(datafile_version));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_group_sendfile_done_deal
  Description: IM模块用户群里面发文件成功后通知
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_sendfile_done_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    struct group_user_msg* pmsg = NULL;
    struct group_user* puser = NULL;
    char group_hid[TOX_ID_STR_LEN+1] = {0};
    char filename[PNR_FILENAME_MAXLEN+1] = {0};
    char filemd5[PNR_MD5_VALUE_MAXLEN+1] = {0};
    char md5[PNR_MD5_VALUE_MAXLEN+1] = {0};
    char fileinfo[PNR_FILEINFO_MAXLEN+1] = {0};
    char fileid[PNR_FILEINFO_MAXLEN+1] = {0};
    char filepath[PNR_FILEPATH_MAXLEN+1] = {0};
    char fullfillpath[PNR_FILEPATH_MAXLEN+1] = {0};
    int uindex= 0,gid = -1,i=0,filesize=0,filetype=0,filenum = 0;
    if(params == NULL)
    {
        return ERROR;
    }
    pmsg = (struct group_user_msg*)malloc(sizeof(struct group_user_msg));
    if(pmsg == NULL)
    {
        return ERROR;
    }
    memset(pmsg,0,sizeof(struct group_user_msg));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",pmsg->from,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",group_hid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileName",filename,PNR_FILENAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileMD5",filemd5,PNR_MD5_VALUE_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileInfo",fileinfo,PNR_FILEINFO_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileSize",filesize,0);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileType",filetype,0);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileId",fileid,PNR_FILEINFO_MAXLEN);
    //参数检查
    uindex = get_indexbytoxid(pmsg->from);
    gid = get_gidbygrouphid(group_hid);
    if(uindex == 0 || group_hid < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_sendfile_done_deal:bad uid(%s) gid(%d)",pmsg->from,group_hid);
        ret_code = PNR_GROUP_SENDFILE_RETURN_BADPRARMS;
    }
    else if(strlen(filename) == 0 || strlen(filemd5) == 0 || filesize <= 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_sendfile_done_deal:bad filename(%s) filemd5(%d) filesize(%d)",filename,filemd5,filesize);
        ret_code = PNR_GROUP_SENDFILE_RETURN_BADPRARMS;
    }
    else 
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
        pmsg->gid = gid;
        pmsg->from_uid = uindex;
        pmsg->type = filetype;
        pmsg->timestamp = (int)time(NULL);
        puser = get_guserbyuindex(gid,uindex);
        if(puser == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad userid(%d)",pmsg->from);
            ret_code = PNR_GROUP_SENDFILE_RETURN_BADPRARMS;
        }
        else
        {
            if(head->api_version >= PNR_API_VERSION_V5)
            {
                filenum = atoi(fileid);
                PNR_REAL_FILEPATH_GET(filepath,uindex,PNR_FILE_SRCFROM_GROUPSEND,filenum,gid,(char*)NULL);
                strcpy(fullfillpath,WS_SERVER_INDEX_FILEPATH);
                strcat(fullfillpath,filepath);
            }
            else
            {
                snprintf(fullfillpath,PNR_FILEPATH_MAXLEN,"%s/%sg%d/%s",WS_SERVER_INDEX_FILEPATH,PNR_GROUP_DATA_PATH,gid,filename);
            }
            if(md5_hash_file(fullfillpath, md5) == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get file(%s) failed",fullfillpath);
                ret_code = PNR_GROUP_SENDFILE_RETURN_BADPRARMS;
            }
            else if(strncasecmp(md5,filemd5,PNR_MD5_VALUE_MAXLEN) != OK)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get file(%s) md5(%s) but input md5(%s)",fullfillpath,md5,filemd5);
                ret_code = PNR_GROUP_SENDFILE_RETURN_BADMD5;
            }
            else
            {
                snprintf(pmsg->ext2,PNR_GROUP_EXTINFO_MAXLEN,"%s:%d:%s",fileid,filesize,filemd5);
                if(strlen(fileinfo) > 0)
                {
                    strcat(pmsg->ext2,":");
                    strcat(pmsg->ext2,fileinfo);
                }                
                if(head->api_version >= PNR_API_VERSION_V5)
                {
                    strcpy(pmsg->ext1,filepath);
                }
                else
                {
                    snprintf(pmsg->ext1,PNR_GROUP_EXTINFO_MAXLEN,"/%sg%d/%s",PNR_GROUP_DATA_PATH,gid,filename);
                }
                strcpy(pmsg->msgpay,filename);
                ret_code = PNR_GROUPSENDMSG_RETCODE_OK;
                pthread_mutex_lock(&g_grouplock[gid]);
                g_grouplist[gid].last_msgid++;
                pmsg->msgid = g_grouplist[gid].last_msgid;
                strcpy(pmsg->group_name,g_grouplist[gid].group_name);
                //插入数据库
                pnr_groupmsg_dbinsert(gid,uindex,pmsg->msgid,filetype,
                    pmsg->from,pmsg->msgpay,"",pmsg->ext1,pmsg->ext2,NULL);
                for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
                {
                    if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != uindex)
                    {
                        pmsg->to_uid = g_grouplist[gid].user[i].userindex;
                        strcpy(pmsg->to,g_grouplist[gid].user[i].toxid);
                        strcpy(pmsg->to_key,g_grouplist[gid].user[i].userkey);
                        if(strlen(g_grouplist[gid].user[i].group_remarks) > 0)
                        {
                            memset(pmsg->group_name,0,PNR_GROUP_USERKEY_MAXLEN);
                            strcpy(pmsg->group_name,g_grouplist[gid].user[i].group_remarks);
                        }
                        im_pushmsg_callback(pmsg->to_uid,PNR_IM_CMDTYPE_GROUPMSGPUSH,TRUE,head->api_version,(void *)pmsg);
                    }
                }
                pthread_mutex_unlock(&g_grouplock[gid]);
            }
        }
    }

    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        free(pmsg);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPSENDFILEDONE));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(pmsg->from));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(pmsg->msgid));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(group_hid));
    if(puser != NULL && strlen(puser->group_remarks) > 0)
    {
        cJSON_AddItemToObject(ret_params, "Gname", cJSON_CreateString(puser->group_remarks));
    }
    else
    {
        cJSON_AddItemToObject(ret_params, "Gname", cJSON_CreateString(g_grouplist[gid].group_name));
    }
    cJSON_AddItemToObject(ret_params, "FileName", cJSON_CreateString(filename));
    cJSON_AddItemToObject(ret_params, "FileId", cJSON_CreateString(fileid));
    cJSON_AddItemToObject(ret_params, "FileType", cJSON_CreateNumber(filetype));
    if(strlen(md5) > 0)
    {
        cJSON_AddItemToObject(ret_params, "FileMD5", cJSON_CreateString(md5));
    }
    if(ret_code == OK)
    {
        cJSON_AddItemToObject(ret_params, "UserKey", cJSON_CreateString(puser->userkey));
    }
    if(head->repeat_flag == TRUE)
    {
        cJSON_AddItemToObject(ret_params, "Repeat", cJSON_CreateNumber(TRUE));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    free(pmsg);
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_group_config_deal
  Description: IM模块用户群配置修改
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_group_config_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char userid[TOX_ID_STR_LEN+1] = {0};
    char targetid[TOX_ID_STR_LEN+1] = {0};
    char groupid[TOX_ID_STR_LEN+1] = {0};
    char name[PNR_USERNAME_MAXLEN+1] = {0};
    int cmdtype = 0,uindex= 0,verify = 0,gid = 0,i = 0;
    struct group_user* p_guser = NULL;
    struct group_sys_msg group_sysmsg;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",userid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"GId",groupid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Type",cmdtype,0);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"ToId",targetid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Name",name,PNR_USERNAME_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"NeedVerify",verify,0);

    memset(&group_sysmsg,0,sizeof(group_sysmsg));
    //参数检查
    switch(cmdtype)
    {
        case GROUP_CONFIG_CMDTYPE_SET_GNAME:
            gid =get_gidbygrouphid(groupid);
            if(gid < 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad groupid(%s)",groupid);
                ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
            }
            else if(strlen(name) == 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad name");
                ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
            }
            else
            {
                uindex = get_indexbytoxid(userid);
                p_guser = get_guserbytoxid(gid,userid);
                if(p_guser == NULL || uindex == 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad userid(%s)",userid);
                    ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
                }
                else if(p_guser->type == GROUP_USER_NORMAL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad userid(%s)",userid);
                    ret_code = GROUP_CONFIG_RETCODE_NOPOWER;
                }
                else
                {
                    if(*plws_index == 0)
                    {
                        *plws_index = uindex;
                    }
                    ret_code = GROUP_CONFIG_RETCODE_OK;
                    pthread_mutex_lock(&g_grouplock[gid]);
                    if(strcmp(name,g_grouplist[gid].group_name) != OK)
                    {
                        memset(g_grouplist[gid].group_name,0,PNR_USERNAME_MAXLEN);
                        strcpy(g_grouplist[gid].group_name,name);
                        pnr_groupname_dbupdate_bygid(gid,g_grouplist[gid].group_name);
                        memset(&group_sysmsg,0,sizeof(group_sysmsg));
                        group_sysmsg.type = GROUP_SYSMSG_REMARK_GROUPNAME;
                        group_sysmsg.gid= gid;
                        strcpy(group_sysmsg.group_hid,groupid);
                        group_sysmsg.from_uid = uindex;
                        strcpy(group_sysmsg.from_user,userid);
                        strcpy(group_sysmsg.msgpay,name);
                        for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
                        {
                            if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != uindex)
                            {
                                group_sysmsg.to_uid = g_grouplist[gid].user[i].userindex;
                                strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[i].toxid);
                                im_pushmsg_callback(g_grouplist[gid].user[i].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                            }
                        }
                    }
                    pthread_mutex_unlock(&g_grouplock[gid]);
                }
            }
            break;
        case GROUP_CONFIG_CMDTYPE_SET_VERIFY:
            uindex = get_indexbytoxid(userid);
            gid =get_gidbygrouphid(groupid);
            if(gid < 0 || uindex == 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad groupid(%s)",groupid);
                ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
            }
            else if(verify != TRUE && verify !=FALSE)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad verify(%d)",verify);
                ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
            }
            else
            {
                if(*plws_index == 0)
                {
                    *plws_index = uindex;
                }
                p_guser = get_guserbytoxid(gid,userid);
                if(p_guser == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad userid(%s)",userid);
                    ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
                }
                else if(p_guser->type == GROUP_USER_NORMAL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad userid(%s)",userid);
                    ret_code = GROUP_CONFIG_RETCODE_NOPOWER;
                }
                else
                {
                    ret_code = GROUP_CONFIG_RETCODE_OK;
                    pthread_mutex_lock(&g_grouplock[gid]);
                    if(verify != g_grouplist[gid].verify)
                    {
                        g_grouplist[gid].verify = verify;
                        pnr_groupverify_dbupdate_bygid(gid,verify);
                    }
                    pthread_mutex_unlock(&g_grouplock[gid]);
                }
            }
            break;
        case GROUP_CONFIG_CMDTYPE_KICKUSER:
            gid =get_gidbygrouphid(groupid);
            if(gid < 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad groupid(%s)",groupid);
                ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
            }
            else if(strlen(targetid) != TOX_ID_STR_LEN)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad ToId(%s)",targetid);
                ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
            }
            else
            {
                uindex = get_indexbytoxid(userid);
                p_guser = get_guserbytoxid(gid,userid);
                if(p_guser == NULL || uindex == 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad userid(%s) uindex(%d)",userid,uindex);
                    ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
                }
                else if(p_guser->type == GROUP_USER_NORMAL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad userid(%s) no power",userid);
                    ret_code = GROUP_CONFIG_RETCODE_NOPOWER;
                }
                else
                {   
                    if(*plws_index == 0)
                    {
                        *plws_index = uindex;
                    }
                    p_guser = get_guserbytoxid(gid,targetid);
                    if(p_guser == NULL)
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad targetid(%s)",targetid);
                        ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
                    }
                    else if(p_guser->type != GROUP_USER_NORMAL)
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:targetid(%s) type(%d)",targetid,p_guser->type);
                        ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
                    }
                    else
                    {
                        ret_code = GROUP_CONFIG_RETCODE_OK;
                        pthread_mutex_lock(&g_grouplock[gid]);
                        memset(&group_sysmsg,0,sizeof(group_sysmsg));
                        group_sysmsg.type = GROUP_SYSMSG_KICKOFF;
                        group_sysmsg.gid= gid;
                        strcpy(group_sysmsg.group_hid,groupid);
                        group_sysmsg.from_uid = uindex;
                        strcpy(group_sysmsg.from_user,userid);
                        group_sysmsg.to_uid = p_guser->userindex;
                        strcpy(group_sysmsg.to_user,targetid);
                        for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
                        {
                            if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != uindex)
                            {
                                strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[i].toxid);
                                im_pushmsg_callback(g_grouplist[gid].user[i].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                            }
                        }
                        pthread_mutex_unlock(&g_grouplock[gid]);
                        pnr_group_deluser(gid,targetid);
                        pnr_groupoper_dbget_insert(gid,GROUP_OPER_ACTION_DELUSER,group_sysmsg.from_uid,group_sysmsg.to_uid,g_grouplist[gid].group_name,
                            group_sysmsg.from_user,group_sysmsg.to_user,"group kickout");
                    }
                }
            }
            break;
        case GROUP_CONFIG_CMDTYPE_SET_GREMARK:
            uindex = get_indexbytoxid(userid);
            gid =get_gidbygrouphid(groupid);
            if(gid < 0 || uindex == 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad groupid(%s)",groupid);
                ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
            }
            else if(strlen(name) == 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad name");
                ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
            }
            else
            {
                if(*plws_index == 0)
                {
                    *plws_index = uindex;
                }
                p_guser = get_guserbytoxid(gid,userid);
                if(p_guser == NULL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad userid(%s)",userid);
                    ret_code = GROUP_CONFIG_RETCODE_BADPARAMS;
                }
                /*else if(p_guser->type == GROUP_USER_NORMAL)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad userid(%s)",userid);
                    ret_code = GROUP_CONFIG_RETCODE_NOPOWER;
                }*/
                else
                {
                    ret_code = GROUP_CONFIG_RETCODE_OK;
                    pthread_mutex_lock(&g_grouplock[gid]);
                    if(strcmp(name,p_guser->group_remarks) != OK)
                    {
                        memset(p_guser->group_remarks,0,PNR_USERNAME_MAXLEN);
                        strcpy(p_guser->group_remarks,name);
                        pnr_groupuser_gremark_dbupdate_byid(gid,p_guser->userindex,name);
                    }
                    pthread_mutex_unlock(&g_grouplock[gid]);
                }
            }
            break;
        case GROUP_CONFIG_CMDTYPE_SET_USERREMARK:
        case GROUP_CONFIG_CMDTYPE_SET_GNICKNAME:
            break;
        default:
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_config_deal:bad cmd(%d)",cmdtype);
            break;
    }
    
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_GROUPCONFIG));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "GId", cJSON_CreateString(groupid));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(userid));
    cJSON_AddItemToObject(ret_params, "Type", cJSON_CreateNumber(cmdtype));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      im_file_rename_deal
  Description: 文件重命名设置
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_file_rename_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    struct im_sendmsg_msgstruct* msg = NULL;
    char uid[TOX_ID_STR_LEN+1] = {0};
    char filename[PNR_FILENAME_MAXLEN+1] = {0};
    char newname[PNR_FILENAME_MAXLEN+1] = {0};
    char newfilepath[PNR_FILEPATH_MAXLEN+1] = {0};
    char newfullpath[PNR_FILEPATH_MAXLEN+1] = {0};
    char* ptmp = NULL;
    int index = 0;
    char cmd[CMD_MAXLEN+1] = {0};
    int msgid = 0;
    if(params == NULL)
    {
        return ERROR;
    }
    msg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",uid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgId",msgid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Filename",filename,PNR_FILENAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Rename",newname,PNR_FILENAME_MAXLEN);
    //uid检查
    index = get_indexbytoxid(uid);
    if(index == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_rename_deal bad uid(%s)",uid);
        ret_code = PNR_RESETDEVNAME_RETCODE_BADRID;
    }
    else if(strlen(filename) == 0 || strlen(newname) == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_rename_deal input para err");
        ret_code = PNR_FILERENAME_RETCODE_BADFILENAME;
    }
    else if(strcmp(filename,newname) == OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_file_rename_deal: filename(%s) no changed",filename);
        ret_code = PNR_FILERENAME_RETCODE_OK;
    }
    else
    {
        if(*plws_index == 0)
        {
            *plws_index = index;
        }
        //根据msgid查找对应数据库记录
        pnr_msglog_dbget_byid(index,msgid,msg);
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_file_rename_deal:get msgid(%d) file(%s) path(%s)",msg->db_id,msg->msg_buff,msg->ext);
        if(head->api_version >= PNR_API_VERSION_V5)
        {
            //新版本只修改逻辑名
            if(strcmp(newname,msg->msg_buff) != OK)
            {
                pnr_msglog_dbupdate_filename_byid(index,msg->db_id,newname,msg->ext);
            }
            ret_code = PNR_FILERENAME_RETCODE_OK;
        }
        else
        {

            ptmp = strrchr(msg->ext,'/');
            if(ptmp == NULL || strstr(msg->ext,filename) == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_rename_deal get filepath(%s) err",msg->ext);
                ret_code = PNR_FILERENAME_RETCODE_BADFILENAME;
            }
            else
            {
                strncpy(newfilepath,msg->ext,(ptmp-msg->ext)+1);
                strcat(newfilepath,newname);
                snprintf(newfullpath,PNR_FILEPATH_MAXLEN,"%s%s",DAEMON_PNR_USERDATA_DIR,newfilepath);
                if(access(newfullpath,F_OK) == OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_rename_deal get targetfile(%s) exsit",newfullpath);
                    ret_code = PNR_FILERENAME_RETCODE_RENEWFILEEXSIT;
                }
                else
                {
                    snprintf(cmd,CMD_MAXLEN,"mv %s%s %s%s",DAEMON_PNR_USERDATA_DIR,msg->ext,DAEMON_PNR_USERDATA_DIR,newfilepath);
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_file_rename_deal:cmd(%s)",cmd);
                    system(cmd);
                    pnr_msglog_dbupdate_filename_byid(index,msg->db_id,newname,newfilepath);
                    ret_code = PNR_FILERENAME_RETCODE_OK;
                }
            }
        }
    }
    free(msg);
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_FILERENAME));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msgid));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(uid));
    cJSON_AddItemToObject(ret_params, "Filename", cJSON_CreateString(newname));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_file_forward_deal
  Description:  文件转发
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_file_forward_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    struct im_sendmsg_msgstruct* msg = NULL;
    char from[TOX_ID_STR_LEN+1] = {0};
    char to[TOX_ID_STR_LEN+1] = {0};
    char filename[PNR_FILENAME_MAXLEN+1] = {0};
    char filepath[PNR_FILEPATH_MAXLEN+1] = {0};
    char srcpath[PNR_FILEPATH_MAXLEN+1] = {0};
    char src_ext[PNR_FILEPATH_MAXLEN+1] = {0};
    char targetpath[PNR_FILEPATH_MAXLEN+1] = {0};
    char dstkey[PNR_AES_CBC_KEYSIZE+1] = {0};
    char* ptmp = NULL;
    char* ptar_filename = NULL;
    int fromid = 0,toid = 0;
    char cmd[CMD_MAXLEN+1] = {0};
    int msgid = 0;
    struct group_user_msg* pgmsg = NULL;
    int gid = -1;
    int filesize=0,i=0,filetype=0;
    char filemd5[PNR_MD5_VALUE_MAXLEN+1] = {0};
    char fileinfo[PNR_FILEINFO_MAXLEN+1] = {0};
    if(params == NULL)
    {
        return ERROR;
    }
    msg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(*msg));
    if (!msg) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }

    //解析参数
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgId",msgid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FromId",from,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"ToId",to,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileName",filename,PNR_FILENAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileKey",dstkey,PNR_AES_CBC_KEYSIZE);
    if(head->api_version >= PNR_API_VERSION_V5)
    {
        CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FilePath",filepath,PNR_FILEPATH_MAXLEN);
    }
    //参数检查
    if(strlen(from) != TOX_ID_STR_LEN || strlen(to) != TOX_ID_STR_LEN || strcmp(from,to) == OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_forward_deal input uid err(%s:%s)",from,to);
        ret_code = PNR_FILEFORWARD_RETCODE_BADUID;
    }
    else if(strlen(filename) == 0 || strlen(dstkey) == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_forward_deal: input params err");
        ret_code = PNR_FILEFORWARD_RETCODE_BADFILENAME;
    }
    else
    {
        if(head->api_version < PNR_API_VERSION_V5)
        {
            strcpy(msg->ext,filepath);
            ptar_filename = strrchr(filepath,'/');
            if(ptar_filename)
            {
                ptar_filename++;
            }
        }
        else
        {
            ptmp = strrchr(filename,'/');
            if(ptmp != NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_file_forward_deal:input filename(%s)",filename);
                strcpy(msg->ext,ptmp+1);
                memset(filename,0,PNR_FILENAME_MAXLEN);
                strcpy(filename,msg->ext);
            }
        }
        
        fromid = get_indexbytoxid(from);
        if(fromid == 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_forward_deal: input params err");
            ret_code = PNR_FILEFORWARD_RETCODE_BADFILENAME;
        }
        else
        {
            if(*plws_index == 0)
            {
                *plws_index = fromid;
            }
            pnr_msglog_dbget_byid(fromid,msgid,msg);
            //原始文件目录
            if(strstr(msg->ext,DAEMON_PNR_USERDATA_DIR) == NULL)
            {
                snprintf(srcpath,PNR_FILEPATH_MAXLEN,"%s%s",DAEMON_PNR_USERDATA_DIR,msg->ext);
            }
            else
            {
                strcpy(srcpath,msg->ext);
            }
            strcpy(src_ext,msg->ext);
            ptmp = strchr(srcpath,',');
            if(ptmp)
            {
                ptmp[0] = 0;
                ptmp++;
                strcpy(fileinfo,ptmp);
            }
            #if 0
            if(strstr(msg->ext,filename) == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_forward_deal: input params err");
                ret_code = PNR_FILEFORWARD_RETCODE_BADFILENAME;
            }
            else
            #endif
            if(access(srcpath,F_OK) != OK)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_file_forward_deal get newfilepath(%s) not exsit",srcpath);
                ret_code = PNR_FILEFORWARD_RETCODE_BADFILENAME;
            }
            else
            {
                filetype = msg->msgtype;
                //转发到群
                if(strncasecmp(to,PNR_GROUPID_PREFIX,PNR_GROUPID_PREFIX_LEN) == OK)
                {
                    gid = get_gidbygrouphid(to);
                    if(gid>=0)
                    {
                        //本地拷贝一份文件备份
                        if(head->api_version >= PNR_API_VERSION_V5)
                        {
                            if(ptar_filename)
                            {
                                snprintf(targetpath,PNR_FILEPATH_MAXLEN,"%s%sg%d/%s",DAEMON_PNR_USERDATA_DIR,PNR_GROUP_DATA_PATH,gid,ptar_filename);
                            }
                        }
                        else
                        {
                            snprintf(targetpath,PNR_FILEPATH_MAXLEN,"%s%sg%d/%s",DAEMON_PNR_USERDATA_DIR,PNR_GROUP_DATA_PATH,gid,filename);
                        }
                        snprintf(cmd,CMD_MAXLEN,"cp -f %s %s",srcpath,targetpath);                    
                        system(cmd);
                        DEBUG_PRINT(DEBUG_LEVEL_INFO,"forward file: local cmd(%s)",cmd);
                    }
                    pgmsg = (struct group_user_msg*)malloc(sizeof(struct group_user_msg));
                    if(pgmsg == NULL)
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"malloc failed");
                        return ERROR;
                    }
                    memset(pgmsg,0,sizeof(struct group_user_msg));
                    pgmsg->gid = gid;
                    strcpy(pgmsg->from,from);
                    pgmsg->from_uid = fromid;
                    pgmsg->type = msg->msgtype;
                    pgmsg->timestamp = (int)time(NULL);
                    filesize = get_file_size(targetpath);
                    md5_hash_file(targetpath, filemd5);
                    snprintf(pgmsg->ext2,PNR_GROUP_EXTINFO_MAXLEN,"%s:%d:%s",filename,filesize,filemd5);
                    if(strlen(fileinfo) > 0)
                    {
                        strcat(pgmsg->ext2,":");
                        strcat(pgmsg->ext2,fileinfo);
                    }
                    if(head->api_version >= PNR_API_VERSION_V5)
                    {
                        snprintf(pgmsg->ext1,PNR_GROUP_EXTINFO_MAXLEN,"/%sg%d/%s",PNR_GROUP_DATA_PATH,gid,ptar_filename);
                    }
                    else
                    {
                        snprintf(pgmsg->ext1,PNR_GROUP_EXTINFO_MAXLEN,"/%sg%d/%s",PNR_GROUP_DATA_PATH,gid,filename);
                    }
                    strcpy(pgmsg->msgpay,filename);
                    ret_code = PNR_GROUPSENDMSG_RETCODE_OK;
                    pthread_mutex_lock(&g_grouplock[gid]);
                    g_grouplist[gid].last_msgid++;
                    pgmsg->msgid = g_grouplist[gid].last_msgid;
                    strcpy(pgmsg->group_name,g_grouplist[gid].group_name);
                    strcpy(pgmsg->file_key,dstkey);
                    //插入数据库
                    pnr_groupmsg_dbinsert(gid,fromid,pgmsg->msgid,pgmsg->type,
                        pgmsg->from,pgmsg->msgpay,"",pgmsg->ext1,pgmsg->ext2,pgmsg->file_key);
                    //消息推送
                    for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
                    {
                        if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != fromid)
                        {
                            pgmsg->to_uid = g_grouplist[gid].user[i].userindex;
                            strcpy(pgmsg->to,g_grouplist[gid].user[i].toxid);
                            strcpy(pgmsg->to_key,g_grouplist[gid].user[i].userkey);
                            if(strlen(g_grouplist[gid].user[i].group_remarks) > 0)
                            {
                                memset(pgmsg->group_name,0,PNR_GROUP_USERKEY_MAXLEN);
                                strcpy(pgmsg->group_name,g_grouplist[gid].user[i].group_remarks);
                            }
                            im_pushmsg_callback(pgmsg->to_uid,PNR_IM_CMDTYPE_GROUPMSGPUSH,TRUE,head->api_version,(void *)pgmsg);
                        }
                    }
                    pthread_mutex_unlock(&g_grouplock[gid]);
                    ret_code = PNR_FILEFORWARD_RETCODE_OK;
                    free(pgmsg);
                }
                else
                {
                    toid = get_indexbytoxid(to);
                    //本地对象
                    if(toid)
                    {
                        //本地拷贝一份文件备份
                        if(head->api_version >= PNR_API_VERSION_V5)
                        {
                            if(ptar_filename)
                            {
                                snprintf(targetpath,PNR_FILEPATH_MAXLEN,"/user%d/r/%s",toid,ptar_filename);
                            }
                        }
                        else
                        {
                            snprintf(targetpath,PNR_FILEPATH_MAXLEN,"/user%d/r/%s",toid,filename);
                        }
                        snprintf(cmd,CMD_MAXLEN,"cp -f %s %s%s",srcpath,DAEMON_PNR_USERDATA_DIR,targetpath);                    
                        DEBUG_PRINT(DEBUG_LEVEL_INFO,"forward file: local cmd(%s)",cmd);
                        system(cmd);
                        if(strcmp(from,msg->fromuser_toxid) == OK)
                        {
                            //如果是自己发送的文件，转发给别人,只需要更新dstkey和toid
                            memset(msg->touser_toxid,0,TOX_ID_STR_LEN);
                            strcpy(msg->touser_toxid,to);
                            memset(msg->prikey,0,sizeof(PNR_RSA_KEY_MAXLEN));
                            strcpy(msg->prikey,dstkey);
                        }
                        else// if(strcmp(from,msg->touser_toxid) == OK)
                        {
                            //如果是自己接收的文件，则原本的dstkey变成新的srckey，然后更新dstkey为新的
                            memset(msg->fromuser_toxid,0,TOX_ID_STR_LEN);
                            strcpy(msg->fromuser_toxid,from);
                            memset(msg->touser_toxid,0,TOX_ID_STR_LEN);
                            strcpy(msg->touser_toxid,to);
                            memset(msg->sign,0,PNR_RSA_KEY_MAXLEN);
                            strcpy(msg->sign,msg->prikey);
                            memset(msg->prikey,0,sizeof(PNR_RSA_KEY_MAXLEN));
                            strcpy(msg->prikey,dstkey);
                        }
                        //数据库插入一条记录，这个本地需不需要再保存一次转发的记录，待定
#if 1
                        memset(msg->ext,0,PNR_FILEPATH_MAXLEN);
                        strcpy(msg->ext,targetpath);

                        if(msg->msgtype == PNR_IM_MSGTYPE_IMAGE || msg->msgtype == PNR_IM_MSGTYPE_AUDIO 
                            ||msg->msgtype == PNR_IM_MSGTYPE_MEDIA || msg->msgtype == PNR_IM_MSGTYPE_FILE)
                        {
                            pnr_msglog_getid(fromid, &msg->log_id);
                            pnr_msglog_dbupdate(fromid, msg->msgtype, msg->log_id,MSG_STATUS_SENDOK,msg->fromuser_toxid,
                                msg->touser_toxid, msg->msg_buff, msg->sign, msg->prikey, src_ext, msg->ext2);
                            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"forward:user(%d) log_id(%d)",fromid,msg->log_id);
                        }
#endif
                        //调用推送消息
                        im_pushmsg_callback(toid, PNR_IM_CMDTYPE_FILEFORWARD_PUSH, TRUE, PNR_API_VERSION_V1,msg);
                        ret_code = PNR_FILEFORWARD_RETCODE_OK;
                    }
                    else
                    {
                        ret_code = PNR_FILEFORWARD_RETCODE_TARGETNOTONLINE;
                    }
                }
            }
        }
    }
    free(msg);
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_FILEFORWARD));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "MsgId", cJSON_CreateNumber(msgid));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(to));
    cJSON_AddItemToObject(ret_params, "FromId", cJSON_CreateString(from));
    if(ret_code == PNR_FILEFORWARD_RETCODE_OK)
    {
        cJSON_AddItemToObject(ret_params, "FileType", cJSON_CreateNumber(filetype));
        if(gid >= 0)
        {
            struct group_user* puser = NULL;
            pthread_mutex_lock(&g_grouplock[gid]);
            puser = get_guserbyuindex(gid,fromid);
            if(puser)
            {
                cJSON_AddItemToObject(ret_params, "ToKey", cJSON_CreateString(puser->userkey));
                cJSON_AddItemToObject(ret_params, "ToUserName", cJSON_CreateString(g_grouplist[gid].group_name));
            } 
            pthread_mutex_unlock(&g_grouplock[gid]);
        }
        else
        {
            struct pnr_account_struct friend_account;
            memset(&friend_account,0,sizeof(friend_account));
            strcpy(friend_account.toxid,to);
            pnr_account_dbget_byuserid(&friend_account);
            cJSON_AddItemToObject(ret_params, "ToKey", cJSON_CreateString(friend_account.user_pubkey));
            cJSON_AddItemToObject(ret_params, "ToUserName", cJSON_CreateString(friend_account.nickname));
        }
    }
    
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_upload_avatar_deal
  Description:  用户上传头像
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_upload_avatar_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    struct pnr_userinfo_struct newuser;
    struct pnr_userinfo_struct srcuser;
    char* ptmp = NULL;
    char fullname[PNR_FILEPATH_MAXLEN+1] = {0};
    char filename[PNR_FILENAME_MAXLEN+1] = {0};
    char curavatar[PNR_FILEPATH_MAXLEN+1] = {0};
    char newavatar[PNR_FILEPATH_MAXLEN+1] = {0};
    char md5string[PNR_MD5_VALUE_MAXLEN+1] = {0};
    char cmd[CMD_MAXLEN+1] = {0};
    int index = 0;
    int changeflag = FALSE;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    memset(&newuser,0,sizeof(newuser));
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Uid",newuser.userid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileName",newuser.avatar,PNR_FILENAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileMD5",newuser.md5,PNR_MD5_VALUE_MAXLEN);
    //参数检查
    index = get_indexbytoxid(newuser.userid);
    if(index == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_upload_avatar_deal input uid err(%d:%s)",index,newuser.userid);
        ret_code = PNR_UPLOAD_AVATAR_RETCODE_BADUID;
    }
    else
    {
        //检测目标文件是否存在
        ptmp = strrchr(newuser.avatar,'/');
        if(ptmp != NULL)
        {
            strcpy(filename,ptmp+1);
        }
        else
        {
            strcpy(filename,newuser.avatar);
        }    
        snprintf(fullname,PNR_FILEPATH_MAXLEN,"%s%s%s",DAEMON_PNR_USERDATA_DIR,PNR_FILECACHE_DIR,filename);
        //检测文件是否存在和文件md5
        if(md5_hash_file(fullname,md5string) == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_upload_avatar_deal input file(%s) not exsit",newuser.avatar);
            ret_code = PNR_UPLOAD_AVATAR_RETCODE_FILENAME_ERR;
        }
        else if(strncasecmp(md5string,newuser.md5,PNR_MD5_VALUE_MAXLEN) != OK)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_upload_avatar_deal input md5(%s) realmd5(%s)",newuser.md5,md5string);
            ret_code = PNR_UPLOAD_AVATAR_RETCODE_BUTT;
        }
        else
        {
            if(*plws_index == 0)
            {
                *plws_index = index;
            }
            //根据userid获取当前系统已有的头像
            memset(&srcuser,0,sizeof(srcuser));
            strcpy(srcuser.userid,newuser.userid);
            pnr_userinfo_dbget_byuserid(&srcuser);
            if(srcuser.id == 0)
            {
                changeflag = TRUE;
            }
            else if(strncasecmp(srcuser.md5,newuser.md5,PNR_MD5_VALUE_MAXLEN) != OK)
            {
                changeflag = TRUE;
            }
            else
            {
                snprintf(curavatar,PNR_FILEPATH_MAXLEN,"%s%s%s",DAEMON_PNR_USERDATA_DIR,PNR_AVATAR_DIR,srcuser.avatar);
                if(access(curavatar,F_OK) != OK)
                {
                    changeflag = TRUE;
                }
            }
            if(changeflag == FALSE)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_upload_avatar_deal avatar no  changed");
                ret_code = PNR_UPLOAD_AVATAR_RETCODE_NOCHANGE;
            }
            else
            {
                unlink(curavatar);
                newuser.local = TRUE;
                newuser.index = index;
                strcpy(newuser.devid,g_daemon_tox.user_toxid);
                memset(newuser.avatar,0,PNR_FILENAME_MAXLEN);
                strcpy(newuser.avatar,filename);
                strcpy(newuser.info,g_imusr_array.usrnode[index].user_nickname);
                //同步数据库
                pnr_userinfo_dbupdate(&newuser);
                snprintf(newavatar,PNR_FILEPATH_MAXLEN,"%s%s",PNR_AVATAR_DIR,filename);
                snprintf(cmd,CMD_MAXLEN,"mv %s %s%s",fullname,DAEMON_PNR_USERDATA_DIR,newavatar);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_upload_avatar_deal:cmd(%s)",cmd);
                system(cmd);
                ret_code = PNR_UPLOAD_AVATAR_RETCODE_OK;
            }
        }
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
	cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_UPLOADAVATAR));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(newuser.userid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_updata_avatar_deal
  Description:  用户更新指定用户头像
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_updata_avatar_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    struct pnr_userinfo_struct userinfo;
    char from[PNR_FILEPATH_MAXLEN+1] = {0};
    char to[PNR_FILEPATH_MAXLEN+1] = {0};
    char fullname[PNR_FILEPATH_MAXLEN+1] = {0};
    char filename[PNR_FILENAME_MAXLEN+1] = {0};
    char md5string[PNR_MD5_VALUE_MAXLEN+1] = {0};
    int fromid = 0,toid = 0;
    int filesize = FALSE;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Uid",from,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Fid",to,PNR_FILENAME_MAXLEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Md5",md5string,PNR_MD5_VALUE_MAXLEN);
    //参数检查
    fromid = get_indexbytoxid(from);
    toid = get_indexbytoxid(to);
    if(fromid == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_updata_avatar_deal input uid err(%s)",from);
        ret_code = PNR_UPDATE_AVATAR_RETCODE_BADUID;
    }
    else if(toid == 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_updata_avatar_deal input fid err(%s)",to);
        ret_code = PNR_UPDATE_AVATAR_RETCODE_BADUID;
    }
    else
    {
        if(*plws_index == 0)
        {
            *plws_index = fromid;
        }
        //获取系统当前的头像信息
        memset(&userinfo,0,sizeof(userinfo));
        strcpy(userinfo.userid,to);
        pnr_userinfo_dbget_byuserid(&userinfo);
        if(userinfo.id == 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_updata_avatar_deal user(%s) avatar no exsit",to);
            ret_code = PNR_UPDATE_AVATAR_RETCODE_FILE_NOEXSIT; 
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_updata_avatar_deal get user(%d:%d:%s),avatar(%s:%s),info(%s)",
                userinfo.index,userinfo.local,userinfo.userid,userinfo.avatar,userinfo.md5,userinfo.info);
            //检测目标文件是否存在
            snprintf(filename,PNR_FILENAME_MAXLEN,"/%s%s",PNR_AVATAR_DIR,userinfo.avatar);
            snprintf(fullname,PNR_FILEPATH_MAXLEN,"%s%s",PNR_AVATAR_FULLDIR,userinfo.avatar);
            if(access(fullname,F_OK) != OK)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_updata_avatar_deal user(%s) avatar no exsit",to);
                ret_code = PNR_UPDATE_AVATAR_RETCODE_FILE_NOEXSIT; 
            }
            else if(strncasecmp(md5string,userinfo.md5,PNR_MD5_VALUE_MAXLEN) == OK)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_updata_avatar_deal user(%s) md5(%s) same",to,md5string);
                ret_code = PNR_UPDATE_AVATAR_RETCODE_NOCHANGE; 
                filesize = im_get_file_size(fullname);
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"input md5(%s),db md5(%s)",md5string,userinfo.md5);
                filesize = im_get_file_size(fullname);
                if(filesize <= 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_updata_avatar_deal user avatar(%s) fillsize err",fullname);
                    ret_code = PNR_UPDATE_AVATAR_RETCODE_FILE_NOEXSIT; 
                }
                else
                {
                    ret_code = PNR_UPDATE_AVATAR_RETCODE_OK; 
                }
            }
        }
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_UPDATEAVATAR));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(from));
    if(ret_code == PNR_UPDATE_AVATAR_RETCODE_OK || ret_code == PNR_UPDATE_AVATAR_RETCODE_NOCHANGE)
    {
        cJSON_AddItemToObject(ret_params, "FileSize", cJSON_CreateNumber(filesize));
        cJSON_AddItemToObject(ret_params, "FileName", cJSON_CreateString(filename));
        cJSON_AddItemToObject(ret_params, "FileMD5", cJSON_CreateString(userinfo.md5));
        if(toid)
        {
            struct pnr_account_struct src_account;
            memset(&src_account,0,sizeof(src_account));
            strcpy(src_account.toxid,to);
            pnr_account_dbget_byuserid(&src_account);
            cJSON_AddItemToObject(ret_params, "TargetKey", cJSON_CreateString(src_account.user_pubkey));
        }
        else
        {
            toid = get_friendid_bytoxid(fromid,to);
            if(toid)
            {
                cJSON_AddItemToObject(ret_params, "TargetKey", cJSON_CreateString(g_imusr_array.usrnode[fromid].friends[toid].user_pubkey));
            }
        }
    }
    cJSON_AddItemToObject(ret_params, "TargetId", cJSON_CreateString(to));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",*retmsg_len);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_pull_tmpaccount_deal
  Description: IM拉取临时账户信息
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_pull_tmpaccount_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char userid[TOX_ID_STR_LEN+1] = {0};
    int uindex = 0;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"userid",userid,TOX_ID_STR_LEN);

    //参数检查
    uindex = get_indexbytoxid(userid);
    if(uindex < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_cmd_template_deal:bad uid(%s)",userid);
        ret_code = PNR_GROUPVERIFY_RETCODE_BADGID;
    }
    else
    {
        if(*plws_index == 0)
        {
            *plws_index = uindex;
        }
    }

    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_PULLTMPACCOUNT));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "ToId", cJSON_CreateString(userid));
    cJSON_AddItemToObject(ret_params, "UserSN", cJSON_CreateString(g_account_array.temp_user_sn));
    cJSON_AddItemToObject(ret_params, "Qrcode", cJSON_CreateString(g_account_array.temp_user_qrcode));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_account_delete_deal
  Description: 节点管理者删除圈子用户
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_account_delete_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char fromid[TOX_ID_STR_LEN+1] = {0};
    char cmd[CMD_MAXLEN+1] = {0};
    int uindex= 0,gid = 0,i = 0;
    struct pnr_account_struct account;
    struct group_sys_msg group_sysmsg;
    if(params == NULL)
    {
        return ERROR;
    }
    memset(&account,0,sizeof(account));
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"From",fromid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"To",account.toxid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Sn",account.user_sn,PNR_USN_MAXLEN);

    //参数检查
    if(strcmp(fromid,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].toxid) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad fromid(%s) need(%s)",fromid,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].toxid);
        ret_code = PNR_ACCOUNT_DELETE_RETURN_NOOWNER;
    }
    else
    {
        if(*plws_index == 0)
        {
            uindex = get_indexbytoxid(fromid);
            if(uindex)
            {
                *plws_index = uindex;
            }
        }
        uindex = get_indexbytoxid(account.toxid);
        if(uindex <= 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad toid(%s)",account.toxid);
            ret_code = PNR_ACCOUNT_DELETE_RETURN_BADUSER;
        }
        else if(uindex == PNR_ADMINUSER_PSN_INDEX)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad toid(%s) adminuser",account.toxid);
            ret_code = PNR_ACCOUNT_DELETE_RETURN_BADUSER;
        }
        else
        {
            pthread_mutex_lock(&g_pnruser_lock[uindex]);
            if(g_imusr_array.usrnode[uindex].user_onlinestatus == TRUE)
            {
                //如果该用户在线，推送消息，通知用户logout
                if(g_imusr_array.usrnode[uindex].user_online_type == USER_ONLINE_TYPE_LWS)
                {
                    pnr_relogin_pushbylws(uindex,PNR_PUSHLOGOUT_REASON_DELUSER);
                }
                else
                {
                    pnr_relogin_pushbytox(uindex,PNR_PUSHLOGOUT_REASON_DELUSER);
                }
            }
            //接着停止对应的tox实例
            if(g_imusr_array.usrnode[uindex].init_flag == TRUE)
            {
                //不通过pthread_cancel
                g_imusr_array.usrnode[uindex].tox_status = PNR_TOX_STATUS_TRYTOEXIT;
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"set user(%d) tox_status(%d)",uindex,g_imusr_array.usrnode[uindex].tox_status);
            }
            //检查该用户是不是当前有群主身份，如果有，要对应解散该群组
            for(gid = 0;gid <PNR_GROUP_MAXNUM;gid++)
            {
                if(uindex == g_grouplist[gid].ownerid)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"user(%d:%s) gid(%d) dissovle",uindex,account.toxid,gid);
                    memset(&group_sysmsg,0,sizeof(group_sysmsg));
                    pthread_mutex_lock(&g_grouplock[gid]);
                    group_sysmsg.gid= gid;
                    strcpy(group_sysmsg.group_hid,g_grouplist[gid].group_hid);
                    group_sysmsg.from_uid = uindex;
                    strcpy(group_sysmsg.from_user,account.toxid);
                    group_sysmsg.type = GROUP_SYSMSG_SELFOUT;
                    strcpy(group_sysmsg.msgpay,g_grouplist[gid].group_name);
                    for(i=0;i<PNR_GROUP_USER_MAXNUM;i++)
                    {
                        if(g_grouplist[gid].user[i].userindex != 0 && g_grouplist[gid].user[i].userindex != uindex)
                        {
                            group_sysmsg.to_uid = g_grouplist[gid].user[i].userindex;
                            strcpy(group_sysmsg.msgtargetuser,g_grouplist[gid].user[i].toxid);
                            im_pushmsg_callback(g_grouplist[gid].user[i].userindex,PNR_IM_CMDTYPE_GROUPSYSPUSH,TRUE,head->api_version,(void *)&group_sysmsg);
                        }
                    }
                    pthread_mutex_unlock(&g_grouplock[gid]);
                    pnr_groupoper_dbget_insert(gid,GROUP_OPER_ACTION_GOWNERDEL,uindex,uindex,g_grouplist[gid].group_name,account.toxid,account.toxid,"del owner user ,dissolve group");
                    pnr_group_dissolve(gid);
                }
            }
            //删除该用户数据
            pnr_usr_instance_dbdelete_bytoxid(account.toxid);
            pnr_friend_delete_bytoxid(account.toxid);
            pnr_userdev_mapping_dbdelte_byusrid(account.toxid);
            pnr_account_dbdelete_byuserid(account.toxid);
            pnr_userinfo_dbdelete_byuserid(account.toxid);
            pnr_tox_datafile_dbdelete_bytoxid(account.toxid);
            if(g_msglogdb_handle[uindex] != NULL)
            {
                sqlite3_close(g_msglogdb_handle[uindex]);
                g_msglogdb_handle[uindex] = NULL;
            }
            if(g_msgcachedb_handle[uindex] == NULL)
            {
                sqlite3_close(g_msgcachedb_handle[uindex]);
                g_msgcachedb_handle[uindex] = NULL;
            }
            if(strlen(g_imusr_array.usrnode[uindex].userdata_pathurl) > strlen(DAEMON_PNR_USERDATA_DIR))
            {
                snprintf(cmd,CMD_MAXLEN,"rm -rf %s/*",g_imusr_array.usrnode[uindex].userdata_pathurl);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_account_delete_deal:system1(%s)",cmd);
                system(cmd);
            }
            if(strlen(g_imusr_array.usrnode[uindex].userinfo_pathurl) > strlen(DAEMON_PNR_USERINFO_DIR))
            {
                memset(cmd,0,CMD_MAXLEN);
                snprintf(cmd,CMD_MAXLEN,"rm -rf %s/*",g_imusr_array.usrnode[uindex].userinfo_pathurl);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_account_delete_deal:system2(%s)",cmd);
                system(cmd);
            }
            i = 0;
            while(g_imusr_array.usrnode[uindex].tox_status != PNR_TOX_STATUS_EXITED)
            {
                sleep(1);
                i++;
                if(i > 5)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%d) tox not exit",uindex);
                    break;
                }
            }
#if 0
            g_imusr_array.usrnode[uindex].init_flag = FALSE;
            g_imusr_array.usrnode[uindex].appactive_flag = FALSE;
            g_imusr_array.usrnode[uindex].user_index = 0;
            g_imusr_array.usrnode[uindex].user_onlinestatus = FALSE;
            g_imusr_array.usrnode[uindex].user_online_type = FALSE;
            g_imusr_array.usrnode[uindex].friendnum = 0;
            g_imusr_array.usrnode[uindex].heartbeat_count = 0;
            g_imusr_array.usrnode[uindex].appid = 0;
            g_imusr_array.usrnode[uindex].hashid = 0;
            g_imusr_array.usrnode[uindex].msglog_dbid = 0;
            g_imusr_array.usrnode[uindex].cachelog_dbid = 0;
            g_imusr_array.usrnode[uindex].tox_tid = 0;
            g_imusr_array.usrnode[uindex].lastmsgid_index = 0;
            g_imusr_array.usrnode[uindex].ptox_handle = NULL;
            g_imusr_array.usrnode[uindex].pss = NULL;
            memset(&g_imusr_array.usrnode[uindex].cache_msgid,0,sizeof(long)*IM_MSGID_CACHENUM);
            memset(&g_imusr_array.usrnode[uindex].u_hashstr,0,PNR_USER_HASHID_MAXLEN);
            memset(&g_imusr_array.usrnode[uindex].user_toxid,0,TOX_ID_STR_LEN);
            memset(&g_imusr_array.usrnode[uindex].user_name,0,PNR_USERNAME_MAXLEN);
            memset(&g_imusr_array.usrnode[uindex].user_nickname,0,PNR_USERNAME_MAXLEN);
            memset(&g_imusr_array.usrnode[uindex].user_pubkey,0,PNR_USER_PUBKEY_MAXLEN);
            memset(&g_imusr_array.usrnode[uindex].userdata_pathurl,0,PNR_FILEPATH_MAXLEN);
            memset(&g_imusr_array.usrnode[uindex].userinfo_pathurl,0,PNR_FILEPATH_MAXLEN);
            memset(&g_imusr_array.usrnode[uindex].userinfo_fullurl,0,PNR_FILEPATH_MAXLEN);
            memset(&g_imusr_array.usrnode[uindex].friends,0,sizeof(struct im_friends_struct)*(PNR_IMUSER_FRIENDS_MAXNUM+1));
            memset(&g_imusr_array.usrnode[uindex].file,0,sizeof(struct im_sendfile_struct)*PNR_MAX_SENDFILE_NUM);
#else
            if(i < 5)
            {
                g_imusr_array.usrnode[uindex].init_flag = FALSE;
                g_imusr_array.usrnode[uindex].appactive_flag = FALSE;
                g_imusr_array.usrnode[uindex].user_onlinestatus = FALSE;
                g_imusr_array.usrnode[uindex].user_online_type = FALSE;
                g_imusr_array.usrnode[uindex].friendnum = 0;
                g_imusr_array.usrnode[uindex].heartbeat_count = 0;
                g_imusr_array.usrnode[uindex].appid = 0;
                g_imusr_array.usrnode[uindex].ptox_handle = NULL;
                memset(&g_imusr_array.usrnode[uindex].cache_msgid,0,sizeof(long)*IM_MSGID_CACHENUM);
                memset(&g_imusr_array.usrnode[uindex].u_hashstr,0,PNR_USER_HASHID_MAXLEN);
                memset(&g_imusr_array.usrnode[uindex].user_toxid,0,TOX_ID_STR_LEN);
                memset(&g_imusr_array.usrnode[uindex].user_name,0,PNR_USERNAME_MAXLEN);
                memset(&g_imusr_array.usrnode[uindex].user_nickname,0,PNR_USERNAME_MAXLEN);
                memset(&g_imusr_array.usrnode[uindex].user_pubkey,0,PNR_USER_PUBKEY_MAXLEN);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"use(%d:%s) delete ok",uindex,account.toxid);
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%d) tox_status(%d) not exit",uindex,g_imusr_array.usrnode[uindex].tox_status);
            }
#endif
            g_imusr_array.cur_user_num --;
            pthread_mutex_unlock(&g_pnruser_lock[uindex]);
            ret_code = PNR_ACCOUNT_DELETE_RETURN_OK;
        }
    }
    
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_DELUSER));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "From", cJSON_CreateString(fromid));
    cJSON_AddItemToObject(ret_params, "To", cJSON_CreateString(account.toxid));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_cmd_rnode_pmsg_deal
  Description: IM rid节点发送过来的消息处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_cmd_rnode_pmsg_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char to_id[TOX_ID_STR_LEN+1] = {0};
    char from_rid[TOX_ID_STR_LEN+1] = {0};
    char userid[TOX_ID_STR_LEN+1] = {0};
    char friendid[TOX_ID_STR_LEN+1] = {0};;
    char info[PNR_ATTACH_INFO_MAXLEN+1] = {0};
    struct pnrdev_register_info devinfo;
    struct pnrdev_netconn_info netinfo;
    int cmd = 0,uindex = 0,f_id = 0,pid = 0,f_num = 0;
    pthread_t task_id = 0;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FromRid",from_rid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"ToRid",to_id,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Cmd",cmd,0);

    //参数检查
    if(strcmp(to_id,g_daemon_tox.user_toxid) != OK)
    {
        ret_code = PNR_RNODEMSG_RETCODE_BADRID;
        snprintf(info,PNR_ATTACH_INFO_MAXLEN,"bad rid(%s)",to_id);
    }
    else if(cmd < PNR_SYSDEBUG_CMDTYPE_CHECK_SYSINFO || cmd >= PNR_SYSDEBUG_CMDTYPE_BUTT)
    {
        ret_code = PNR_RNODEMSG_RETCODE_BADPARAMS;
        snprintf(info,PNR_ATTACH_INFO_MAXLEN,"bad cmdtype(%d)",cmd);
    }
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_SYSDERLYCMD));
    cJSON_AddItemToObject(ret_params, "Cmd", cJSON_CreateNumber(cmd));
    cJSON_AddItemToObject(ret_params, "Rid", cJSON_CreateString(g_daemon_tox.user_toxid));
    if(ret_code == OK)
    {
        switch(cmd)
        {
            case PNR_SYSDEBUG_CMDTYPE_CHECK_SYSINFO:
                memset(&netinfo,0,sizeof(netinfo));
                pnr_devinfo_get(&devinfo);
                ret_code = PNR_RNODEMSG_RETCODE_OK;
                snprintf(info,PNR_ATTACH_INFO_MAXLEN,"devinfo:devtype(%d),e0mac(%s)version(%s)",
                    devinfo.dev_type,devinfo.eth0_localip,devinfo.version);
                break;
            case PNR_SYSDEBUG_CMDTYPE_CHECK_NETINFO:
                memset(&netinfo,0,sizeof(netinfo));
                pnr_netconfig_dbget(&netinfo);
                if(netinfo.pubnet_mode == PNRDEV_NETCONN_FRPPROXY)
                {
                    pnr_check_process_byname("frpc",&pid);
                    if(pid <= 0)
                    {
                        netinfo.conn_status = FALSE;
                    }
                    else if(pnr_check_frp_connstatus() == TRUE)
                    {
                        netinfo.conn_status = TRUE;
                    }
                    else
                    {
                        netinfo.conn_status = FALSE;
                    }
                }
                ret_code = PNR_RNODEMSG_RETCODE_OK;
                snprintf(info,PNR_ATTACH_INFO_MAXLEN,"netinfo:pubmode(%d),frpmode(%d)pub_ip(%s:%d)c_status(%d)",
                        netinfo.pubnet_mode,netinfo.frp_mode,netinfo.pub_ipstr,netinfo.pnr_port,netinfo.conn_status);
                break;
            case PNR_SYSDEBUG_CMDTYPE_REFLUSH_DEVREG:
                if(g_pnrdevtype == PNR_DEV_TYPE_ONESPACE)
                {
                    if (pthread_create(&task_id, NULL, pnr_dev_register_task, NULL) != 0) 
                    {
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pthread_create post_devinfo_register failed");
                    }
                    ret_code = PNR_RNODEMSG_RETCODE_OK;
                    strcpy(info,"reflush devreg ok");
                }
                else
                {
                    ret_code = PNR_RNODEMSG_RETCODE_BADPARAMS;
                    snprintf(info,PNR_ATTACH_INFO_MAXLEN,"bad devtype(%d)",g_pnrdevtype);
                }
                break;
            case PNR_SYSDEBUG_CMDTYPE_POST_DEBUGINFO:
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"User",userid,TOX_ID_STR_LEN);
                uindex = get_indexbytoxid(userid);
                if(uindex < 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_cmd_rnode_pmsg_deal:bad uid(%s)",userid);
                    ret_code = PNR_RNODEMSG_RETCODE_BADPARAMS;
                    snprintf(info,PNR_ATTACH_INFO_MAXLEN,"bad userid(%s)",userid);
                }
                else
                {
                    ret_code = PNR_RNODEMSG_RETCODE_OK;
                }
                break;
            case PNR_SYSDEBUG_CMDTYPE_ACTIVE_USER:
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"User",userid,TOX_ID_STR_LEN);
                uindex = get_indexbytoxid(userid);
                if(uindex < 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_cmd_rnode_pmsg_deal:bad uid(%s)",userid);
                    ret_code = PNR_RNODEMSG_RETCODE_BADPARAMS;
                    snprintf(info,PNR_ATTACH_INFO_MAXLEN,"bad userid(%s)",userid);
                }
                else
                {
                    int run = 0;
                    if(g_imusr_array.usrnode[uindex].init_flag == FALSE)
                    {
                        pthread_mutex_lock(&g_pnruser_lock[uindex]);
                        //DEBUG_PRINT(DEBUG_LEVEL_INFO,"index(%d) init_flag(%d)",index,g_imusr_array.usrnode[index].init_flag);                
                        //g_tmp_instance_index = index;
                        if (pthread_create(&g_imusr_array.usrnode[uindex].tox_tid, NULL, imstance_daemon, &uindex) != 0) 
                        {
                            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
                            ret_code = PNR_RNODEMSG_RETCODE_OTHERERR;
                            snprintf(info,PNR_ATTACH_INFO_MAXLEN,"userid(%s) active failed",userid);
                        } 
                        else
                        {
                            while(g_imusr_array.usrnode[uindex].init_flag != TRUE && run < PNR_TOXINSTANCE_CREATETIME)
                            {
                                sleep(1);
                                run++;
                            }
                            if(run >= PNR_TOXINSTANCE_CREATETIME)
                            {
                                ret_code = PNR_RNODEMSG_RETCODE_OTHERERR;
                                snprintf(info,PNR_ATTACH_INFO_MAXLEN,"userid(%s) active failed",userid);
                            }
                            else
                            {
                                ret_code = PNR_RNODEMSG_RETCODE_OK;
                                snprintf(info,PNR_ATTACH_INFO_MAXLEN,"userid(%s) active ok",userid);
                                DEBUG_PRINT(DEBUG_LEVEL_INFO,"renew user(%s)",g_imusr_array.usrnode[uindex].user_toxid);
                                //如果是新用户激活对应数据库句柄
                                if(g_msglogdb_handle[uindex] == NULL)
                                {
                                     if (sql_msglogdb_init(uindex) != OK) 
                                     {
                                         DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msglog db failed",uindex);
                                     }
                                }
                                if(g_msgcachedb_handle[uindex] == NULL)
                                {
                                     if (sql_msgcachedb_init(uindex) != OK) 
                                     {
                                         DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msgcache db failed",uindex);
                                     }
                                }
                            }
                        }
                        pthread_mutex_unlock(&g_pnruser_lock[uindex]);
                    }
                    else
                    {
                        ret_code = PNR_RNODEMSG_RETCODE_OK;
                        snprintf(info,PNR_ATTACH_INFO_MAXLEN,"userid(%s) active already",userid);
                    }
                }
                break;
            case PNR_SYSDEBUG_CMDTYPE_CHECK_FRIENDS:
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"User",userid,TOX_ID_STR_LEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Friend",friendid,TOX_ID_STR_LEN);
                uindex = get_indexbytoxid(userid);
                if(uindex < 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_cmd_rnode_pmsg_deal:bad uid(%s)",userid);
                    ret_code = PNR_RNODEMSG_RETCODE_BADPARAMS;
                    snprintf(info,PNR_ATTACH_INFO_MAXLEN,"bad userid(%s)",userid);
                }
                else
                {
                    f_id = get_friendid_bytoxid(uindex,friendid);
                    ret_code = PNR_RNODEMSG_RETCODE_OK;
                    snprintf(info,PNR_ATTACH_INFO_MAXLEN,"user(%d:%s) friend(%s) f_id(%d)",
                        uindex,userid,friendid,f_id);
                }
                break;
            case PNR_SYSDEBUG_CMDTYPE_CHECKANDADD_FRIENDS:
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"User",userid,TOX_ID_STR_LEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Friend",friendid,TOX_ID_STR_LEN);
                uindex = get_indexbytoxid(userid);
                if(uindex < 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_cmd_rnode_pmsg_deal:bad uid(%s)",userid);
                    ret_code = PNR_RNODEMSG_RETCODE_BADPARAMS;
                    snprintf(info,PNR_ATTACH_INFO_MAXLEN,"bad userid(%s)",userid);
                }
                else
                {
                    f_id = get_friendid_bytoxid(uindex,friendid);
                    f_num = check_and_add_friends(g_tox_linknode[uindex], friendid, 
                            g_imusr_array.usrnode[uindex].userinfo_fullurl);
                    ret_code = PNR_RNODEMSG_RETCODE_OK;
                    snprintf(info,PNR_ATTACH_INFO_MAXLEN,"user(%d:%s) friend(%d:%s) f_num(%d)",
                        uindex,userid,f_id,friendid,f_num);
                }
                break;
             default:
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad cmdtype(%d)",cmd);
                break;   
        }
    }
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_params, "Info", cJSON_CreateString(info));
   
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_cmd_rnode_msgreply_deal
  Description: IM模块根节点返回消息处理函数
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_cmd_rnode_msgreply_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    char userid[TOX_ID_STR_LEN+1] = {0};
    char info[PNR_ATTACH_INFO_MAXLEN+1] = {0};
    int cmd = 0,ret = 0;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Rid",userid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Info",info,PNR_ATTACH_INFO_MAXLEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Cmd",cmd,0);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"RetCode",ret,0);
    DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"Rnode(%s) cmd(%d) ret(%d) info(%s)",userid,cmd,ret,info);
    return OK;
}

/**********************************************************************************
  Function:      im_cmd_template_deal
  Description: IM模块消息处理模板函数
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_cmd_template_deal(cJSON * params,char* retmsg,int* retmsg_len,
	int* plws_index, struct imcmd_msghead_struct *head)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    int ret_code = 0;
    char* ret_buff = NULL;
    char userid[TOX_ID_STR_LEN+1] = {0};
    int result = 0,uindex= 0;
    if(params == NULL)
    {
        return ERROR;
    }
    //解析参数
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"From",userid,TOX_ID_STR_LEN);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Result",result,0);

    //参数检查
    uindex = get_indexbytoxid(userid);
    if(uindex < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_group_verify_deal:bad uid(%s)",userid);
        ret_code = PNR_GROUPVERIFY_RETCODE_BADGID;
    }
    else 
    {
        if(result)
        {}
    }
    //构建响应消息
    cJSON * ret_root = cJSON_CreateObject();
    cJSON * ret_params = cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)head->api_version));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)head->msgid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_VERIFYGROUP));
    cJSON_AddItemToObject(ret_params, "RetCode", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    
    *retmsg_len = strlen(ret_buff);
    if(*retmsg_len < TOX_ID_STR_LEN || *retmsg_len >= IM_JSON_MAXLEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d,%s)",*retmsg_len,ret_buff);
        free(ret_buff);
        return ERROR;
    }
    strcpy(retmsg,ret_buff);
    free(ret_buff);
    return OK;
}
/**********************************************************************************
  Function:      im_rcvmsg_deal
  Description: IM模块消息处理函数
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int im_rcvmsg_deal(struct per_session_data__minimal *pss,char* pmsg,
	int msg_len,char* retmsg,int* retmsg_len,int* ret_flag,int* plws_index)
{
    cJSON *root = NULL;
    cJSON *params = NULL;
    struct imcmd_msghead_struct msg_head;
	//int fileindex = 0;

	//接收文件的socket，暂时不用
	#if 0
	if (*plws_index > 0 && pss->fd > 0) {
		DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "pssfd:%d-%d-%d", msg_len, *plws_index, pss->fd);
		printf("pssfd:%d-%d-%d\n", msg_len, *plws_index, pss->fd);
		fileindex = im_sendfile_get_node_byfd(pss->fd, *plws_index);
		if (fileindex >= 0) {
			DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "im_rcvmsg_deal: recv file(%d-%d)", pss->user_index, msg_len);
			printf("im_rcvmsg_deal: recv file(%d-%d)\n", pss->user_index, msg_len);
			return im_rcv_file_deal(pmsg, msg_len, retmsg, retmsg_len, ret_flag, *plws_index, fileindex);
		}
	}
	#endif
	
    if(pmsg == NULL)
    {
        return ERROR;
    }
	
    root = cJSON_Parse(pmsg);
    if(root == NULL) 
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get root failed");
        return ERROR;
    }

	pmsg[msg_len] = 0;	/* 避免打印乱码 */
    DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"id(%d) rec msg(%d):(%s)",pss->user_index,msg_len,pmsg);
    params = cJSON_GetObjectItem(root, "params");
    if (!params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get params failed");
        return ERROR;
    }
	
    memset(&msg_head,0,sizeof(msg_head));
    if(im_msghead_parses(root,params,&msg_head) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_msghead_parses failed");
        return ERROR;
    }

	pthread_mutex_lock(&g_formating_lock);
	if (g_formating == 1 && msg_head.im_cmdtype != PNR_IM_CMDTYPE_REBOOT) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "formating...");
        pthread_mutex_unlock(&g_formating_lock);
		return ERROR;
	}
	pthread_mutex_unlock(&g_formating_lock);
    memset(retmsg,0,IM_JSON_MAXLEN);
    //这里按照api接口版本分批处理
    if(msg_head.api_version == PNR_API_VERSION_V1)
    {
        switch(msg_head.im_cmdtype)
        {
            case PNR_IM_CMDTYPE_LOGIN:
                if(im_userlogin_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userlogin_deal failed");
                    return ERROR;
                }
                g_imusr_array.usrnode[*plws_index].user_online_type = USER_ONLINE_TYPE_LWS;
                break;
            case PNR_IM_CMDTYPE_DESTORY:
                if(im_userdestory_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_userdestory_deal failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_ADDFRIENDREQ:
                if(im_addfriend_req_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
                break; 
            case PNR_IM_CMDTYPE_ADDFRIENDDEAL:
                if(im_addfriend_deal_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_ONLINESTATUSCHECK:
                if(im_onlinestatus_check_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_onlinestatus_check_deal failed");
                    return ERROR;
                }
                break;                
            case PNR_IM_CMDTYPE_DELFRIENDCMD:
                if(im_delfriend_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_delfriend_cmd_deal failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_SENDMSG:
                if(im_sendmsg_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_sendmsg_cmd_deal failed");
                    return ERROR;
                }
                break;  
            case PNR_IM_CMDTYPE_DELMSG:
                if(im_delmsg_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_delmsg_cmd_deal failed");
                    return ERROR;
                }
                break;              
            case PNR_IM_CMDTYPE_HEARTBEAT:
                if(im_heartbeat_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_heartbeat_cmd_deal failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_PULLMSG:
                if(im_pullmsg_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_heartbeat_cmd_deal failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_PULLFRIEND:
                if(im_pullfriend_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_pullfriend_cmd_deal failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_SYNCHDATAFILE:
                if(im_sysch_datafile_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_sysch_datafile_deal failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_PULLFILE:
    			if (im_pullfile_cmd_deal(params, retmsg, retmsg_len, plws_index, &msg_head)) {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_sendfile_cmd_deal failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
            case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
            case PNR_IM_CMDTYPE_DELFRIENDPUSH:
            case PNR_IM_CMDTYPE_PUSHMSG:
            case PNR_IM_CMDTYPE_DELMSGPUSH:
            case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
            case PNR_IM_CMDTYPE_PUSHFILE:
                if(im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,*plws_index) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                    return ERROR;
                }
                break;                
            default:
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad cmdtype(%d)",msg_head.im_cmdtype);
                break;
        }
        //需要回响应的命令
        if(msg_head.im_cmdtype == PNR_IM_CMDTYPE_LOGIN
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_DESTORY
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_ADDFRIENDREQ
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_ADDFRIENDDEAL
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_DELFRIENDCMD
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_SENDMSG
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_DELMSG
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_ONLINESTATUSCHECK
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_HEARTBEAT
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_PULLMSG
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_PULLFRIEND
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_SENDFILE
            || msg_head.im_cmdtype == PNR_IM_CMDTYPE_PULLFILE)
        {
            *ret_flag = TRUE;
        }
    }
    else if(msg_head.api_version == PNR_API_VERSION_V2)
    {
        switch(msg_head.im_cmdtype)
        {
            case PNR_IM_CMDTYPE_CREATENORMALUSER:
                if (im_create_normaluser_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_create_normaluser_cmd_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_LOGIN:
                if (im_userlogin_v2_deal(params,retmsg,retmsg_len,plws_index,&msg_head,-1,pss)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userlogin_v2_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_LOGINIDENTIFY:
                if (im_login_identify_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_login_identify_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_PREREGISTER:
                if (im_user_preregister_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_preregister_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_REGISTER:
                if (im_user_register_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_register_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_RECOVERY:
                if (im_user_recovery_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_recovery_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_RECOVERYIDENTIFY:
                if (im_user_recoveryidentify_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_recoveryidentify_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_READMSG:
                if (im_readmsg_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_readmsg_cmd_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_READMSGPUSH:
                if (im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,*plws_index)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_readmsg_cmd_deal PNR_IM_CMDTYPE_READMSGPUSH failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_USERINFOUPDATE:
                if (im_userinfoupdate_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userinfoupdate_cmd_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_USERINFOPUSH:
                if (im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,*plws_index)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_readmsg_cmd_deal PNR_IM_CMDTYPE_USERINFOPUSH failed");
                    return ERROR;
                }
                break;                
            case PNR_IM_CMDTYPE_PULLUSERLIST:
                if (im_pulluserlist_cmd_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_readmsg_cmd_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_LOGOUT:
                if (im_logout_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_logout_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_CHANGEREMARKS:
                if (im_changeremarks_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_changeremarks_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
			case PNR_IM_CMDTYPE_GET_RELATIONSHIP:
				if (im_get_relationship_status_cmd_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get relationship status failed");
                    return ERROR;
				}
				*ret_flag = TRUE;
                break;
			case PNR_IM_CMDTYPE_PULLFILELIST:
				if (im_pull_file_list_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pull file list failed");
                    return ERROR;
				}
				*ret_flag = TRUE;
                break;
			case PNR_IM_CMDTYPE_UPLOADFILEREQ:
				if (im_upload_file_req_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "upload file req failed");
                    return ERROR;
				}
				*ret_flag = TRUE;
                break;
			case PNR_IM_CMDTYPE_DELETEFILE:
				if (im_delete_file_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "delete file failed");
                    return ERROR;
				}
				*ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_ROUTERLOGIN:
				if (im_routerlogin_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_routerlogin_deal failed");
                    return ERROR;
				}
				*ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_RESETROUTERKEY:
                if (im_reset_routerkey_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_reset_routerkey_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_RESETUSERIDCODE:
                if (im_reset_useridcode_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_reset_useridcode_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;                
            default:
                break;
        }
    }
    else if(msg_head.api_version == PNR_API_VERSION_V3)
    {
        switch(msg_head.im_cmdtype)
        {
            case PNR_IM_CMDTYPE_ADDFRIENDREQ:
                if(im_addfriend_req_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break; 
            case PNR_IM_CMDTYPE_ADDFRIENDDEAL:
                if(im_addfriend_deal_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_SENDMSG:
                if(im_sendmsg_cmd_deal_v3(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_sendmsg_cmd_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLMSG:
                if(im_pullmsg_cmd_deal_v3(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_pullmsg_cmd_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLFRIEND:
                if(im_pullfriend_cmd_deal_v3(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_pullmsg_cmd_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
            case PNR_IM_CMDTYPE_PUSHMSG:
            case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
            case PNR_IM_CMDTYPE_PUSHFILE:
                if(im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,*plws_index) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                    return ERROR;
                }
                break;  
  			case PNR_IM_CMDTYPE_GETDISKDETAILINFO:
                if (im_get_disk_detailinfo_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im get disk info failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GETDISKTOTALINFO:
                if (im_get_disk_totalinfo_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im get disk info failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
			case PNR_IM_CMDTYPE_FORMATDISK:
                if (im_format_disk_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_format_disk_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
			case PNR_IM_CMDTYPE_REBOOT:
                if (im_reboot_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_reboot_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
		    default:
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad api_version(%d) cmd(%d)",msg_head.api_version,msg_head.im_cmdtype);
                break;
        }
    }
    else if(msg_head.api_version == PNR_API_VERSION_V4)
    {
        switch(msg_head.im_cmdtype)
        {
			case PNR_IM_CMDTYPE_RESETROUTERNAME:
                if (im_reset_routername_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_reset_routername_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_REGISTER:
                if (im_user_register_v4_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_format_disk_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_LOGIN:
                if (im_userlogin_v4_deal(params,retmsg,retmsg_len,plws_index,&msg_head,-1,pss,FALSE))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_format_disk_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLFRIEND:
                if (im_pullfriend_cmd_deal_v4(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_format_disk_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_ADDFRIENDREQ:
                if(im_addfriend_req_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break; 
            case PNR_IM_CMDTYPE_ADDFRIENDDEAL:
                if(im_addfriend_deal_deal(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;                
            case PNR_IM_CMDTYPE_RECOVERY:
                if (im_user_recovery_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_recovery_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
            case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
            case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
                if(im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,*plws_index) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_CREATEGROUP:
                if (im_group_create_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_create_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_INVITEGROUP:
                if (im_group_invite_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_invite_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPINVITEDEAL:
                if (im_group_invitedeal_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_invitedeal_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_VERIFYGROUP:
                if (im_group_verify_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_verify_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;                  
            case PNR_IM_CMDTYPE_GROUPQUIT:
                if (im_group_quit_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_quit_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPLISTPULL:
                if (im_group_pulllist_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_pulllist_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPUSERPULL:
                if (im_group_pulluser_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_pulluser_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPMSGPULL:
                if (im_group_pullmsg_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_pullmsg_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPSENDMSG:
                if (im_group_sendmsg_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_sendmsg_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPSENDFILEPRE:
                if (im_group_sendfile_predeal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_sendfile_predeal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPSENDFILEDONE:
                if (im_group_sendfile_done_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_sendfile_done_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPDELMSG:
                if (im_group_delmsg_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_delmsg_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPCONFIG:
                if (im_group_config_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_config_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_FILERENAME:
                if (im_file_rename_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_file_rename_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_FILEFORWARD:
                if (im_file_forward_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_file_forward_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_UPLOADAVATAR:
                if (im_upload_avatar_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_upload_avatar_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_UPDATEAVATAR:
                if (im_updata_avatar_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_updata_avatar_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLTMPACCOUNT:
                if (im_pull_tmpaccount_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_pull_tmpaccount_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;                
            case PNR_IM_CMDTYPE_GROUPINVITEPUSH:
            case PNR_IM_CMDTYPE_VERIFYGROUPPUSH:
            case PNR_IM_CMDTYPE_GROUPSYSPUSH:
            case PNR_IM_CMDTYPE_GROUPMSGPUSH:
                if(im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,*plws_index) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_DELUSER:
                if (im_account_delete_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_account_delete_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            default:
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad api_version(%d) cmd(%d)",msg_head.api_version,msg_head.im_cmdtype);
                break;
        }
    }
    else if(msg_head.api_version == PNR_API_VERSION_V5)
    {
        switch(msg_head.im_cmdtype)
        {
			 case PNR_IM_CMDTYPE_PULLFILE:
    			if (im_pullfile_cmd_deal(params, retmsg, retmsg_len, plws_index, &msg_head)) {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_pullfile_cmd_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLMSG:
                if(im_pullmsg_cmd_deal_v3(params,retmsg,retmsg_len,plws_index,&msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_pullmsg_cmd_deal_v5 failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLFILELIST:
                if (im_pull_file_list_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pull file list failed");
                    return ERROR;
				}
				*ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_DELETEFILE:
				if (im_delete_file_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "delete file failed");
                    return ERROR;
				}
				*ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPMSGPULL:
                if (im_group_pullmsg_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_pullmsg_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPSENDFILEDONE:
                if (im_group_sendfile_done_deal(params,retmsg,retmsg_len,plws_index,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_sendfile_done_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
             case PNR_IM_CMDTYPE_FILERENAME:
                if (im_file_rename_deal(params, retmsg, retmsg_len, plws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_file_rename_deal failed");
                    return ERROR;
                }
                *ret_flag = TRUE;
                break;
            case PNR_IM_CMDTYPE_PUSHFILE:
            case PNR_IM_CMDTYPE_GROUPMSGPUSH:
                if(im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,*plws_index) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                    return ERROR;
                }
                break;
            default:
                break;
        }
    }
    else
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad api_version(%d)",msg_head.api_version);
        return ERROR;
    }
    if(*plws_index > 0 && *plws_index <= PNR_IMUSER_MAXNUM)
    {
        pthread_mutex_lock(&(g_pnruser_lock[*plws_index]));
        if(msg_head.im_cmdtype == PNR_IM_CMDTYPE_LOGOUT)
        {
            g_imusr_array.usrnode[*plws_index].user_onlinestatus = USER_ONLINE_STATUS_OFFLINE;
        }
        else
        {
            g_imusr_array.usrnode[*plws_index].user_onlinestatus = USER_ONLINE_STATUS_ONLINE;
        }
        g_imusr_array.usrnode[*plws_index].heartbeat_count = 0;
        g_imusr_array.usrnode[*plws_index].user_online_type = USER_ONLINE_TYPE_LWS;
        //DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d) online_type(%d)",*plws_index,g_imusr_array.usrnode[*plws_index].user_online_type);
        pthread_mutex_unlock(&(g_pnruser_lock[*plws_index]));
    }
    return OK;
}

char g_tox_retbuf[IM_JSON_MAXLEN] = {0};
/*****************************************************************************
 函 数 名  : im_tox_rcvmsg_deal
 功能描述  : 处理app发送到router的tox消息
 输入参数  : Tox *m         
             char *pmsg     
             int len        
             int friendnum  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月25日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_tox_rcvmsg_deal(Tox *m, char *pmsg, int len, int friendnum)
{
    cJSON *root = NULL;
    cJSON *params = NULL;
    struct imcmd_msghead_struct msg_head;
    int retlen = 0;
    int userindex = 0;
    int needret = 0;
	char sendmsg[1500] = {0};
	int i = 0;
	
    root = cJSON_Parse(pmsg);
    if (!root) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get root failed");
        return ERROR;
    }

	DEBUG_PRINT(DEBUG_LEVEL_NORMAL, "rec app msg(%d):(%s)", len, pmsg);
    params = cJSON_GetObjectItem(root, "params");
    if (!params) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get params failed");
        return ERROR;
    }
	
	memset(&msg_head, 0, sizeof(msg_head));
	msg_head.iftox = TRUE;
	msg_head.friendnum = friendnum;
	
    if (im_msghead_parses(root, params, &msg_head)) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "msg head parse failed");
        return ERROR;
    }

	pthread_mutex_lock(&g_formating_lock);
	if (g_formating == 1 && msg_head.im_cmdtype != PNR_IM_CMDTYPE_REBOOT) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "formating...");
        pthread_mutex_unlock(&g_formating_lock);
		return ERROR;
	}
	pthread_mutex_unlock(&g_formating_lock);
	
	//request other segment
	if (msg_head.offset > 0) {
		tox_seg_msg_process(m, friendnum, &msg_head);
		return OK;
	}
	
	memset(g_tox_retbuf, 0, IM_JSON_MAXLEN);
    if (msg_head.api_version == PNR_API_VERSION_V1) {
        switch (msg_head.im_cmdtype) {
        case PNR_IM_CMDTYPE_LOGIN:
            if (im_userlogin_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userlogin_deal failed");
                return ERROR;
            }
            needret = TRUE;
            g_imusr_array.usrnode[userindex].user_online_type = USER_ONLINE_TYPE_TOX;
            break;
            
        case PNR_IM_CMDTYPE_DESTORY:
            if (im_userdestory_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userdestory_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_ADDFRIENDREQ:
            if (im_addfriend_req_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_addfriend_req_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_ADDFRIENDDEAL:
            if (im_addfriend_deal_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_addfriend_req_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_ONLINESTATUSCHECK:
            if (im_onlinestatus_check_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_onlinestatus_check_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_DELFRIENDCMD:
            if (im_delfriend_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_delfriend_cmd_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_SENDMSG:
            if (im_sendmsg_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_sendmsg_cmd_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_DELMSG:
            if (im_delmsg_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_delmsg_cmd_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_HEARTBEAT:
            if (im_heartbeat_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_heartbeat_cmd_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_PULLMSG:
            if (im_pullmsg_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_heartbeat_cmd_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_PULLFRIEND:
            if (im_pullfriend_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_pullfriend_cmd_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
		case PNR_IM_CMDTYPE_SENDFILE:
			if (im_sendfile_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_sendfile_cmd_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;
            
        case PNR_IM_CMDTYPE_SYNCHDATAFILE:
            if (im_sysch_datafile_deal(params, g_tox_retbuf, &retlen, &userindex ,&msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userlogin_deal failed");
                return ERROR;
            }
            break;
        case PNR_IM_CMDTYPE_PULLFILE:
			if (im_pullfile_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_sendfile_cmd_deal failed");
                return ERROR;
            }
            needret = TRUE;
            break;

        case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
        case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
        case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
        case PNR_IM_CMDTYPE_DELFRIENDPUSH:
        case PNR_IM_CMDTYPE_PUSHMSG:
        case PNR_IM_CMDTYPE_DELMSGPUSH:
        case PNR_IM_CMDTYPE_PUSHFILE:
        case PNR_IM_CMDTYPE_PUSHFILE_TOX:
            if (im_replaymsg_deal(params, msg_head.im_cmdtype, &msg_head, 0)) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                return ERROR;
            }
            break;
            
        default:
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad cmdtype(%d)", msg_head.im_cmdtype);
            break;
        }
    } 
    else if(msg_head.api_version == PNR_API_VERSION_V2)
    {
        switch(msg_head.im_cmdtype)
        {
            case PNR_IM_CMDTYPE_CREATENORMALUSER:
                if (im_create_normaluser_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_create_normaluser_cmd_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_LOGIN:
                if (im_userlogin_v2_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head, friendnum, NULL)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userlogin_v2_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_LOGINIDENTIFY:
                if (im_login_identify_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_login_identify_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_PREREGISTER:
                if (im_user_preregister_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_preregister_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_REGISTER:
                if (im_user_register_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_register_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_RECOVERY:
                if (im_user_recovery_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_recovery_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_RECOVERYIDENTIFY:
                if (im_user_recoveryidentify_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_recoveryidentify_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_READMSG:
                if (im_readmsg_cmd_deal(params,g_tox_retbuf,&retlen,&userindex,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_readmsg_cmd_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_READMSGPUSH:
                if (im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,0)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_replaymsg_deal PNR_IM_CMDTYPE_READMSGPUSH failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_USERINFOUPDATE:
                if (im_userinfoupdate_cmd_deal(params,g_tox_retbuf,&retlen,&userindex,&msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userinfoupdate_cmd_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_USERINFOPUSH:
                if (im_replaymsg_deal(params,msg_head.im_cmdtype,&msg_head,0)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_replaymsg_deal PNR_IM_CMDTYPE_USERINFOPUSH failed");
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_PULLUSERLIST:
                if (im_pulluserlist_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_pulluserlist_cmd_deal PNR_IM_CMDTYPE_PULLUSERLIST failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_LOGOUT:
                if (im_logout_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_logout_deal PNR_IM_CMDTYPE_LOGOUT failed");
                    return ERROR;
                }
                needret = TRUE;
                break;  
            case PNR_IM_CMDTYPE_CHANGEREMARKS:
                if (im_changeremarks_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_changeremarks_deal PNR_IM_CMDTYPE_LOGOUT failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
			case PNR_IM_CMDTYPE_GET_RELATIONSHIP:
				if (im_get_relationship_status_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get relationship status failed");
                    return ERROR;
				}
				needret = TRUE;
                break;
			case PNR_IM_CMDTYPE_PULLFILELIST:
				if (im_pull_file_list_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pull file list failed");
                    return ERROR;
				}
				needret = TRUE;
                break;
			case PNR_IM_CMDTYPE_UPLOADFILEREQ:
				if (im_upload_file_req_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "upload file req failed");
                    return ERROR;
				}
				needret = TRUE;
                break;
			case PNR_IM_CMDTYPE_UPLOADFILE:
				if (im_upload_file_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "upload file failed");
                    return ERROR;
				}
				needret = TRUE;
                break;
			case PNR_IM_CMDTYPE_DELETEFILE:
				if (im_delete_file_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "delete file failed");
                    return ERROR;
				}
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_ROUTERLOGIN:
                if (im_routerlogin_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_routerlogin_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_RESETROUTERKEY:
                if (im_reset_routerkey_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_reset_routerkey_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_RESETUSERIDCODE:
                if (im_reset_useridcode_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_reset_useridcode_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;                 
            default:
                break;
        }
    }
    else if(msg_head.api_version == PNR_API_VERSION_V3)
    {
        switch(msg_head.im_cmdtype)
        {
            case PNR_IM_CMDTYPE_ADDFRIENDREQ:
                if(im_addfriend_req_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break; 
            case PNR_IM_CMDTYPE_ADDFRIENDDEAL:
                if(im_addfriend_deal_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_SENDMSG:
                if(im_sendmsg_cmd_deal_v3(params, g_tox_retbuf, &retlen, &userindex, &msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_sendmsg_cmd_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLMSG:
                if(im_pullmsg_cmd_deal_v3(params, g_tox_retbuf, &retlen, &userindex, &msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_pullmsg_cmd_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLFRIEND:
                if(im_pullfriend_cmd_deal_v3(params, g_tox_retbuf, &retlen, &userindex, &msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_pullmsg_cmd_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
            case PNR_IM_CMDTYPE_PUSHMSG:
            case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
            case PNR_IM_CMDTYPE_PUSHFILE:
                if (im_replaymsg_deal(params, msg_head.im_cmdtype, &msg_head, 0)) {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                    return ERROR;
                }
                break; 
            case PNR_IM_CMDTYPE_GETDISKDETAILINFO:
                if (im_get_disk_detailinfo_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_get_disk_info_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_GETDISKTOTALINFO:
                if (im_get_disk_totalinfo_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_get_disk_info_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
			case PNR_IM_CMDTYPE_FORMATDISK:
                if (im_format_disk_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_format_disk_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
			case PNR_IM_CMDTYPE_REBOOT:
                if (im_reboot_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_reboot_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
		    default:
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad api_version(%d) cmd(%d)",msg_head.api_version,msg_head.im_cmdtype);
                break;
        }
    }   
    else if(msg_head.api_version == PNR_API_VERSION_V4)
    {
        switch(msg_head.im_cmdtype)
        {
			case PNR_IM_CMDTYPE_RESETROUTERNAME:
                if (im_reset_routername_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_reset_routername_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_REGISTER:
                if (im_user_register_v4_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_register_v4_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_LOGIN:
                if (im_userlogin_v4_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head, friendnum, NULL,FALSE))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userlogin_v4_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLFRIEND:
                if (im_pullfriend_cmd_deal_v4(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_pullfriend_cmd_deal_v4 failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_ADDFRIENDREQ:
                if(im_addfriend_req_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break; 
            case PNR_IM_CMDTYPE_ADDFRIENDDEAL:
                if(im_addfriend_deal_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_addfriend_req_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break; 
            case PNR_IM_CMDTYPE_RECOVERY:
                if (im_user_recovery_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_recovery_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
            case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
            case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
                if (im_replaymsg_deal(params, msg_head.im_cmdtype, &msg_head, 0)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                    return ERROR;
                }
                break;
            case PNR_IM_CMDTYPE_FILERENAME:
                if (im_file_rename_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_file_rename_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_FILEFORWARD:
                if (im_file_forward_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_file_forward_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_UPLOADAVATAR:
                if (im_upload_avatar_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_upload_avatar_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_UPDATEAVATAR:
                if (im_updata_avatar_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_updata_avatar_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_CREATEGROUP:
                if (im_group_create_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_create_deal failed");
                    return ERROR;
                }
				needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_INVITEGROUP:
                if (im_group_invite_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_invite_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPINVITEDEAL:
               if (im_group_invitedeal_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                   DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_invitedeal_deal failed");
                   return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_VERIFYGROUP:
               if (im_group_verify_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_verify_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPQUIT:
               if (im_group_quit_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_quit_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPLISTPULL:
               if (im_group_pulllist_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_pulllist_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPUSERPULL:
               if (im_group_pulluser_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_pulluser_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPMSGPULL:
               if (im_group_pullmsg_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_pullmsg_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPSENDMSG:
               if (im_group_sendmsg_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_sendmsg_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPSENDFILEPRE:
               if (im_group_sendfile_predeal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_sendfile_predeal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPSENDFILEDONE:
               if (im_group_sendfile_done_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_sendfile_done_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPDELMSG:
               if (im_group_delmsg_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_delmsg_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_GROUPCONFIG:
               if (im_group_config_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_config_deal failed");
                  return ERROR;
               }
               needret = TRUE;
               break;
           case PNR_IM_CMDTYPE_PULLTMPACCOUNT:
               if (im_pull_tmpaccount_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
               {
                  DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_pull_tmpaccount_deal failed");
                  return ERROR;
               }
               needret = TRUE;
              break;                  
            case PNR_IM_CMDTYPE_GROUPINVITEPUSH:
            case PNR_IM_CMDTYPE_VERIFYGROUPPUSH:
            case PNR_IM_CMDTYPE_GROUPSYSPUSH:
            case PNR_IM_CMDTYPE_GROUPMSGPUSH:
               if (im_replaymsg_deal(params, msg_head.im_cmdtype, &msg_head, 0)) {
                   DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                   return ERROR;
               }
               break;               
            case PNR_IM_CMDTYPE_DELUSER:
                if (im_account_delete_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                   DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_account_delete_deal failed");
                   return ERROR;
                }
                needret = TRUE;
                break;   
            case PNR_IM_CMDTYPE_SYSDEBUGMSG:
                if (im_cmd_rnode_pmsg_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                   DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_account_delete_deal failed");
                   return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_SYSDERLYMSG:
                if (im_cmd_rnode_msgreply_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                   DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_account_delete_deal failed");
                   return ERROR;
                }
                break;
            default:
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad api_version(%d) cmd(%d)",msg_head.api_version,msg_head.im_cmdtype);
                break;
        }
    }
    else if(msg_head.api_version == PNR_API_VERSION_V5)
    {
        switch(msg_head.im_cmdtype)
        {
			 case PNR_IM_CMDTYPE_PULLFILE:
    			if (im_pullfile_cmd_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_pullfile_cmd_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLMSG:
                if(im_pullmsg_cmd_deal_v3(params, g_tox_retbuf, &retlen, &userindex, &msg_head) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_pullmsg_cmd_deal_v5 failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_PULLFILELIST:
                if (im_pull_file_list_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pull file list failed");
                    return ERROR;
				}
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_DELETEFILE:
				if (im_delete_file_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
				{
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "delete file failed");
                    return ERROR;
				}
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPMSGPULL:
                if (im_group_pullmsg_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_pullmsg_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_GROUPSENDFILEDONE:
                if (im_group_sendfile_done_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head)) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_group_sendfile_done_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
             case PNR_IM_CMDTYPE_FILERENAME:
                if (im_file_rename_deal(params, g_tox_retbuf, &retlen, &userindex, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_file_rename_deal failed");
                    return ERROR;
                }
                needret = TRUE;
                break;
            case PNR_IM_CMDTYPE_PUSHFILE:
            case PNR_IM_CMDTYPE_GROUPMSGPUSH:
                if(im_replaymsg_deal(params, msg_head.im_cmdtype, &msg_head, 0) != OK)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_replaymsg_deal cmd(%d) failed",msg_head.im_cmdtype);
                    return ERROR;
                }
                break;
            default:
                break;
        }
    }
    else 
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "bad api_version(%d)", msg_head.api_version);
        return ERROR;
    }

    if (userindex > 0 && userindex <= PNR_IMUSER_MAXNUM) 
    {
		for (i = 0; i <= PNR_IMUSER_MAXNUM; i++) {
			if (g_imusr_array.usrnode[i].appid == friendnum)
				g_imusr_array.usrnode[i].appid = -1;
		}
		g_imusr_array.usrnode[userindex].appid = friendnum;
        pthread_mutex_lock(&(g_pnruser_lock[userindex]));
        if(msg_head.im_cmdtype == PNR_IM_CMDTYPE_LOGOUT)
        {
            g_imusr_array.usrnode[userindex].user_onlinestatus = USER_ONLINE_STATUS_OFFLINE;
        }
        else
        {
            g_imusr_array.usrnode[userindex].user_onlinestatus = USER_ONLINE_STATUS_ONLINE;
        }
        g_imusr_array.usrnode[userindex].user_onlinestatus = USER_ONLINE_STATUS_ONLINE;
        g_imusr_array.usrnode[userindex].heartbeat_count = 0;
        g_imusr_array.usrnode[userindex].user_online_type = USER_ONLINE_TYPE_TOX;
        pthread_mutex_unlock(&(g_pnruser_lock[userindex]));
    }
        
    if (needret) 
    {
    	if (retlen >= MAX_CRYPTO_DATA_SIZE) 
        {
            DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"retlen(%d) MAX_CRYPTO_DATA_SIZE(%d)",retlen,MAX_CRYPTO_DATA_SIZE);
			while (1) {
				cJSON *RspJson = cJSON_Parse(g_tox_retbuf);
				if (!RspJson) {
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "parse retbuf(%s) err!", g_tox_retbuf);
					break;
				}

                cJSON *RspJsonParams = cJSON_GetObjectItem(RspJson, "params");
				if (!RspJsonParams) {
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "parse params(%s) err!", g_tox_retbuf);
					cJSON_Delete(RspJson);
					break;
				}

				char *RspStrParams = cJSON_PrintUnformatted_noescape(RspJsonParams);
				if (!RspStrParams) {
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "print params(%s) err!", g_tox_retbuf);
					cJSON_Delete(RspJson);
					break;
				}
				cJSON *JsonFrame = cJSON_Duplicate(RspJson, true);
			    if (!JsonFrame) {
			        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "dup RspJson err!");
					cJSON_Delete(RspJson);
					free(RspStrParams);
					break;
				}

				cJSON_Delete(RspJson);
				cJSON_DeleteItemFromObject(JsonFrame, "params");

				char *StrFrame = cJSON_PrintUnformatted_noescape(JsonFrame);
				if (!StrFrame) {
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "print frame err!");
					cJSON_Delete(JsonFrame);
					free(RspStrParams);
					break;
				}
				struct tox_msg_send *tmsg = (struct tox_msg_send *)calloc(1, sizeof(*tmsg));
			    if (!tmsg) {
			        cJSON_Delete(JsonFrame);
			        free(StrFrame);
					free(RspStrParams);
			        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err![%d]", errno);
			    	break;
			    }

				tmsg->msg = RspStrParams;
			    tmsg->msgid = msg_head.msgid;
			    tmsg->friendnum = friendnum;
			    tmsg->msglen = strlen(RspStrParams);
				tmsg->offset = MAX_SEND_DATA_SIZE;
				tmsg->recvtime = time(NULL);
			    strncpy(tmsg->frame, StrFrame, sizeof(tmsg->frame) - 1);
				cJSON_Delete(JsonFrame);
		        free(StrFrame);
					
			    pthread_rwlock_wrlock(&g_tox_msg_send_lock);
			    list_add_tail(&tmsg->list, &g_tox_msg_send_list);
			    pthread_rwlock_unlock(&g_tox_msg_send_lock);

				cJSON *RspJsonSend = cJSON_Parse(tmsg->frame);
				memcpy(sendmsg, tmsg->msg, MAX_SEND_DATA_SIZE);
				cJSON_AddStringToObject(RspJsonSend, "params", sendmsg);
            	cJSON_AddNumberToObject(RspJsonSend, "more", 1);
            	cJSON_AddNumberToObject(RspJsonSend, "offset", 0);
				char *RspStrSend = cJSON_PrintUnformatted_noescape(RspJsonSend);
				if (!RspStrSend) {
					DEBUG_PRINT(DEBUG_LEVEL_ERROR, "print RspJsonSend err!");
					cJSON_Delete(RspJsonSend);
					break;
				}
                //DEBUG_PRINT(DEBUG_LEVEL_INFO,"resp(%d:%s)",strlen(RspStrSend),RspStrSend);
				tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, 
            		(uint8_t *)RspStrSend, strlen(RspStrSend), NULL);
				cJSON_Delete(RspJsonSend);
				free(RspStrSend);
				break;
			}
		}
        else
        {
        	tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, 
            	(uint8_t *)g_tox_retbuf, retlen, NULL);
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"resp(%d:%s)",retlen,g_tox_retbuf);
		}
    }

    if (msg_head.forward) 
    {
        //目标用户是本地用户
        if(msg_head.to_userid)
        {
            im_pushmsg_callback(msg_head.to_userid,msg_head.im_cmdtype,TRUE,msg_head.api_version,msg_head.toxmsg);
        }
        else
        {
            im_tox_pushmsg_callback(userindex, msg_head.im_cmdtype,msg_head.api_version,msg_head.toxmsg);
        }
        free(msg_head.toxmsg);
    }
    return OK;
}
/*****************************************************************************
 函 数 名  : im_send_file_by_tox
 功能描述  : 通过tox发送文件
 输入参数  : struct im_user_msg_sendfile *pfile  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月10日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
void im_send_file_by_tox(Tox *tox, struct lws_cache_msg_struct *msg, int push)
{
    int ret = 0;

    ret = imtox_send_file(tox, msg, push);
    if (ret < 0) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "tox send file(%s) failed!", msg->filename);
        msg->filestatus = 0;
    } else {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "tox send file(%s) start!", msg->filename);
    }
    
	return;
}

/*****************************************************************************
 函 数 名  : im_send_file_deal
 功能描述  : 文件接收完成后处理
 输入参数  : struct im_user_msg_sendfile *pfile  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月22日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
void im_send_file_deal(struct im_user_msg_sendfile *pfile)
{
    int from = 0;
    int to = 0;
    int api_version = 0;

    from = get_indexbytoxid(pfile->fromid);
    to = get_indexbytoxid(pfile->toid);
    if(pfile->ver_str != 0)
    {
        api_version = PNR_API_VERSION_V5;
    }
    else
    {
        api_version = PNR_API_VERSION_V1;
    }
    if (to) {
        im_pushmsg_callback(to, PNR_IM_CMDTYPE_PUSHFILE, TRUE, api_version,pfile);
    } else {
        im_pushmsg_callback(from, PNR_IM_CMDTYPE_PUSHFILE, FALSE, api_version,pfile);
    }
}

/*****************************************************************************
 函 数 名  : im_rcv_file_deal_bin
 功能描述  : 接收二进制文件流
 输入参数  : struct per_session_data__minimal *pss  
             char *pmsg                             
             int msg_len                            
             char *retmsg                           
             int *retmsg_len                        
             int *ret_flag                          
             int *plws_index                        
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月8日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
void im_rcv_file_deal_bin(struct per_session_data__minimal_bin *pss, char *pmsg,
	int msg_len, char *retmsg, int *retmsg_len, int *ret_flag, int *plws_index)
{
	struct im_user_msg_sendfile_resp *resp = (struct im_user_msg_sendfile_resp *)retmsg;
	struct im_user_msg_sendfile *pfile = (struct im_user_msg_sendfile *)pmsg;
	uint16_t crcr = ntohs(pfile->crc);
	uint16_t crct = 0;
	int index = 0;
    int gid = 0;
	int fflag = 0;
	char fullfilename[PNR_FILEPATH_MAXLEN+1] = {0};
	char filepath[512] = {0};
    int action = 0;
    char fileinfo[PNR_FILEINFO_MAXLEN+1] = {0};
    char* ptmp = NULL;
    int fileinfo_len =0;
    int srcfrom = 0;
	
    if (!pss->logid) 
    {
        pnr_msglog_getid(pss->user_index, &pss->logid);
    } 
	
	*retmsg_len = sizeof(struct im_user_msg_sendfile_resp);
	*ret_flag = TRUE;
	resp->action = pfile->action;
	resp->fileid = pfile->fileid;
	resp->logid = htonl(pss->logid);
	resp->segseq = pfile->segseq;
    action = ntohl(pfile->action);
	memcpy(resp->fromid, pfile->fromid, TOX_ID_STR_LEN);
	memcpy(resp->toid, pfile->toid, TOX_ID_STR_LEN);
    
    if (strlen(pfile->fromid) < TOX_ID_STR_LEN) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "fromid(%s)-toid(%s) error", pfile->fromid, pfile->toid);
        return;
    }

    if ((pfile->porperty_flag != TRUE) && (action != PNR_IM_MSGTYPE_AVATAR) && (strlen(pfile->srckey) < PNR_USN_MAXLEN))
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "action(%d) porperty_flag(%d) srckey(%s)-dstkey(%s) error", action,pfile->porperty_flag,pfile->srckey, pfile->dstkey);
        return;
    }

	pfile->crc = 0;
	crct = gen_crc16((uint8_t *)pmsg, (unsigned short)sizeof(struct im_user_msg_sendfile));
	//DEBUG_PRINT(DEBUG_LEVEL_ERROR, "%04x-%04x", crct, crcr);
	//if (crcr == crct) {
    if (1) {
		resp->code = 0;
		index = get_indexbytoxid(pfile->fromid);
        if (index == 0) 
        {
            resp->code = htons(2);
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get fromtoxid(%s) failed", 
				pfile->fromid);
        }
        else 
        {
            if (*plws_index == 0) 
            {
                *plws_index = index;
			}
            //文件名中可能附带文件属性信息，预处理
            ptmp = strchr(pfile->filename,PNR_FILEINFO_ATTACH_FLAG);
            if(ptmp != NULL)
            {
                fileinfo_len = pfile->filename+strlen(pfile->filename)-ptmp;
                fileinfo_len = ((fileinfo_len >= PNR_FILEINFO_MAXLEN)?PNR_FILEINFO_MAXLEN:fileinfo_len);
                strncpy(fileinfo,ptmp,fileinfo_len);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"rec filename(%s) fileinfo(%s)",pfile->filename,fileinfo);
                ptmp[0] = 0;
            }
			if (!pss->fd) 
            {
                //添加群文件处理
                if(pfile->porperty_flag != 0 && pfile->porperty_flag != 0x30)//字符的0
                {
                    srcfrom = PNR_FILE_SRCFROM_GROUPSEND;
                    gid = get_gidbygrouphid(pfile->toid);
                    if(gid < 0)
                    {            
                        resp->code = htons(2);
                        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get gid(%s) failed porperty_flag(0x%2x)", pfile->toid,pfile->porperty_flag);
                        return;
                    }
                    else
                    {
					    snprintf(fullfilename, sizeof(fullfilename), "%s%sg%d/%s", 
						    DAEMON_PNR_USERDATA_DIR,PNR_GROUP_DATA_PATH, gid,pfile->filename);
                    }
                }
                else if(action == PNR_IM_MSGTYPE_AVATAR)
                {
                    srcfrom = PNR_FILE_SRCFROM_AVATAR;
					snprintf(fullfilename, sizeof(fullfilename), "%s%s/%s", 
						DAEMON_PNR_USERDATA_DIR,PNR_FILECACHE_DIR, pfile->filename);    
                }
				else if (pfile->toid[0]) {
                    srcfrom = PNR_FILE_SRCFROM_MSGSEND;
					snprintf(fullfilename, sizeof(fullfilename), "%ss/%s", 
						g_imusr_array.usrnode[index].userdata_pathurl, pfile->filename);
				}
                else {
                    srcfrom = PNR_FILE_SRCFROM_SELFUPLOAD;
					snprintf(fullfilename, sizeof(fullfilename), "%su/%s", 
						g_imusr_array.usrnode[index].userdata_pathurl, pfile->filename);
				}
				if (pfile->ifcontinue) {
					fflag = O_CREAT | O_RDWR;
				} 
                else {
					fflag = O_CREAT | O_RDWR | O_TRUNC;
                }
                if(pfile->ver_str == WS_FILELIST_V1)
                {
                    memset(fullfilename,0,PNR_FILEPATH_MAXLEN);
                    strcpy(fullfilename,WS_SERVER_INDEX_FILEPATH);
                    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"get fileid(%d->%d)",pfile->fileid,htonl(pfile->fileid));
                    PNR_REAL_FILEPATH_GET(filepath,index,srcfrom,htonl(pfile->fileid),gid,pfile->filename);
                    strcat(fullfilename,filepath);
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"new rcvfile bin reset filename(%s)",fullfilename);
                }
                pss->fd = open(fullfilename, fflag, 0644);
                DEBUG_PRINT(DEBUG_LEVEL_INFO, "rcv file(%s) start", fullfilename);
            }
			if (!pss->fd)
            {
				resp->code = htons(3);
            	DEBUG_PRINT(DEBUG_LEVEL_ERROR, "open file(%s) failed", pfile->filename);
			} 
            else 
            {
				lseek(pss->fd, ntohl(pfile->offset), SEEK_SET);
				write(pss->fd, pfile->content, ntohl(pfile->segsize));
				DEBUG_PRINT(DEBUG_LEVEL_INFO, "from(%s) srckey(%s) to(%s) dstkey(%s) file(%s) --recv %d--offset:%d content(0x%2x 0x%2x 0x%2x 0x%2x) contentend(0x%2x 0x%2x 0x%2x 0x%2x)", 
					resp->fromid,pfile->srckey,resp->toid,pfile->dstkey,pfile->filename, ntohl(pfile->segsize), ntohl(pfile->offset),
					pfile->content[0],pfile->content[1],pfile->content[2],pfile->content[3],
					pfile->content[ntohl(pfile->segsize)-4],pfile->content[ntohl(pfile->segsize)-3],pfile->content[ntohl(pfile->segsize)-2],pfile->content[ntohl(pfile->segsize)-1]);
				if (!pfile->segmore)
                {
					close(pss->fd);
					pss->fd = 0;
					pss->buflen = 0;
					pss->sfile = 0;
                    //添加群文件处理
                    if(pfile->porperty_flag != 0 && pfile->porperty_flag != 0x30)
                    {
                        //老版本这里只写文件，文件相关记录的保存在GroupSendFileDone中处理
                    }
                    else //普通消息
                    {
                        if(action == PNR_IM_MSGTYPE_AVATAR){
                            srcfrom = PNR_FILE_SRCFROM_AVATAR;
                            snprintf(filepath, sizeof(filepath), "/%s/%s",PNR_FILECACHE_DIR, pfile->filename);
                        }
                        else
                        {
                            if (pfile->toid[0]) 
                            {
                                srcfrom = PNR_FILE_SRCFROM_MSGSEND;
        						snprintf(filepath, sizeof(filepath), "/user%d/s/%s", index, pfile->filename);
        					}
                            else 
        					{
                                srcfrom = PNR_FILE_SRCFROM_SELFUPLOAD;
        						snprintf(filepath, sizeof(filepath), "/user%d/u/%s", index, pfile->filename);
        					}
                        }		
                        if(pfile->ver_str == WS_FILELIST_V1)
                        {
                            PNR_REAL_FILEPATH_GET(filepath,index,srcfrom,htonl(pfile->fileid),gid,pfile->filename);
                            DEBUG_PRINT(DEBUG_LEVEL_INFO,"new rcvfile bin reset filename(%s)",filepath);
                        }
                        //这里用magic传递logid
    					pfile->magic = pss->logid;
                        if(fileinfo_len > 0)
                        {
                            strcat(filepath,fileinfo);
                        }
                        pnr_msglog_dbupdate(index, pss->type, pss->logid, MSG_STATUS_SENDOK, pfile->fromid,
    						pfile->toid, pfile->filename, pfile->srckey, pfile->dstkey, filepath, 
    						ntohl(pfile->offset) + ntohl(pfile->segsize));
    					if (pfile->toid[0]) 
                        {
                            if(fileinfo_len > 0)
                            {
                                strcat(pfile->filename,fileinfo);
                            }
    						im_send_file_deal(pfile);
    					}
    					DEBUG_PRINT(DEBUG_LEVEL_INFO, "rcv file(%s) filepath(%s) complete", pfile->filename,filepath);
			        }
                }
			}
        }
	} 
    else
    {
		resp->code = htons(1);
		DEBUG_PRINT(DEBUG_LEVEL_ERROR, "crc check err(%04x--%04x)",
			crcr, crct); 
	}
	resp->crc = htons(gen_crc16((uint8_t *)retmsg, sizeof(struct im_user_msg_sendfile_resp)));
	//resp->crc = htons(CalculateCRC16((uint8_t *)retmsg, sizeof(struct im_user_msg_sendfile_resp)));
}

/*****************************************************************************
 函 数 名  : im_rcvmsg_deal_bin
 功能描述  : 处理二进制消息
 输入参数  : struct per_session_data__minimal *pss  
             char* pmsg                             
             int msg_len                            
             char* retmsg                           
             int* retmsg_len                        
             int* ret_flag                          
             int* plws_index                        
 输出参数  : 无
 返 回 值  : 每传输一个文件起一个socket,所以此处不考虑连包问题
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月8日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int im_rcvmsg_deal_bin(struct per_session_data__minimal_bin *pss, char *pmsg,
	int msg_len, char *retmsg, int *retmsg_len, int *ret_flag, int *plws_index)
{
	uint32_t *magic = (uint32_t *)pmsg;
	uint32_t *action = (uint32_t *)pmsg + 1;
	struct im_user_msg_sendfile_resp *resp = (struct im_user_msg_sendfile_resp *)retmsg;
	struct im_user_msg_sendfile *pfile = (struct im_user_msg_sendfile *)pss->buf;

	/* first packet */
	if (ntohl(*magic) == IM_MSG_MAGIC) {
		switch (ntohl(*action)) {
		case PNR_IM_MSGTYPE_FILE:
		case PNR_IM_MSGTYPE_IMAGE:
		case PNR_IM_MSGTYPE_AUDIO:
		case PNR_IM_MSGTYPE_MEDIA:
        case PNR_IM_MSGTYPE_AVATAR:
			pss->sfile = 1;
			pss->type = ntohl(*action);
			memcpy(pss->buf, pmsg, msg_len);
			pss->buflen = msg_len;

			pss->user_index = get_indexbytoxid(pfile->fromid);
			if (!pss->user_index) {
				DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get user(%s) err", pfile->fromid);
				return ERROR;
			}

			if (pfile->toid[0] && pfile->porperty_flag == FALSE && !if_friend_available(pss->user_index, pfile->toid)) {
				pss->buflen = 0;
				pss->sfile = 0;
				*retmsg_len = sizeof(struct im_user_msg_sendfile_resp);
				*ret_flag = TRUE;
				resp->action = pfile->action;
				resp->fileid = pfile->fileid;
				resp->segseq = pfile->segseq;
				memcpy(resp->fromid, pfile->fromid, TOX_ID_STR_LEN);
				memcpy(resp->toid, pfile->toid, TOX_ID_STR_LEN);
				resp->code = htons(5);
				resp->crc = htons(gen_crc16((uint8_t *)retmsg, sizeof(struct im_user_msg_sendfile_resp)));
				DEBUG_PRINT(DEBUG_LEVEL_INFO, "user(%d) porperty_flag(%d) not friend(%s)", pss->user_index, pfile->porperty_flag,pfile->toid);
				return OK;
			}
					
			//if (pss->buflen >= sizeof(struct im_user_msg_sendfile)) 
			//DEBUG_PRINT(DEBUG_LEVEL_INFO,"get segmore(%d),buflen(%d)segsize(%d) buf(0x%2x 0x%2x)",pfile->segmore,pss->buflen,ntohl(pfile->segsize),pfile->content[0],pfile->content[1]);
            if (pss->buflen >= (ntohl(pfile->segsize)+ WS_SENDFILE_HEADLEN))
            {
				im_rcv_file_deal_bin(pss, pss->buf, pss->buflen, retmsg, retmsg_len, ret_flag, plws_index);
				pss->buflen = 0;
				pss->sfile = 0;

				return OK;
			} else {
				return 2;
			}

		default:
			DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im msg type err[%d]", *action);
			return ERROR;
		}
	} else {
		if (pss->sfile) {
			if (pss->buflen + msg_len < MAX_FILE_BUFF) {
				memcpy(pss->buf + pss->buflen, pmsg, msg_len);
				pss->buflen += msg_len;
				
				//if (pss->buflen >= sizeof(struct im_user_msg_sendfile)) 
                //DEBUG_PRINT(DEBUG_LEVEL_INFO,"get segmore2(%d),buflen(%d)segsize(%d) buf(0x%2x 0x%2x)",pfile->segmore,pss->buflen,ntohl(pfile->segsize),pfile->content[0],pfile->content[1]);
                if (pss->buflen >= (ntohl(pfile->segsize)+ WS_SENDFILE_HEADLEN))
                {
					im_rcv_file_deal_bin(pss, pss->buf, pss->buflen, retmsg, retmsg_len, ret_flag, plws_index);
					pss->buflen = 0;
					pss->sfile = 0;

					return OK;
				} else {
					return 2;
				}
			} else {
				pss->buflen = 0;
				pss->sfile = 0;
				*retmsg_len = sizeof(struct im_user_msg_sendfile_resp);
				*ret_flag = TRUE;
				resp->action = pfile->action;
				resp->fileid = pfile->fileid;
				resp->segseq = pfile->segseq;
				memcpy(resp->fromid, pfile->fromid, TOX_ID_STR_LEN);
				memcpy(resp->toid, pfile->toid, TOX_ID_STR_LEN);
				resp->code = htons(4);
				resp->crc = htons(gen_crc16((uint8_t *)retmsg, sizeof(struct im_user_msg_sendfile_resp)));
			}
		}
	}

	return OK;
}

/**********************************************************************************
  Function:      imtox_pushmsg_predeal
  Description: IMtox模块消息预处理函数
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int imtox_pushmsg_predeal(int id,char* puser,char* pmsg,int msg_len)
{
    cJSON *root = NULL;
    cJSON *params = NULL;
    cJSON *tmp_item = NULL;
	cJSON *dup = NULL;
	char *tmp_json_buff = NULL;
	int msgid = 0;
    int index = 0;
    char filepath[UPLOAD_FILENAME_MAXLEN] = {0};
    char fileinfo[PNR_FILEINFO_MAXLEN+1] = {0};
    char fullfile[UPLOAD_FILENAME_MAXLEN*2] = {0};
    char* p_realfilename = NULL;
    struct imcmd_msghead_struct msg_head;
    struct im_friend_msgstruct friend;
    struct im_sendfile_struct sendfile;
    struct im_userdev_mapping_struct devinfo;
    struct im_sendmsg_msgstruct* psendmsg = NULL;

    if(pmsg == NULL)
    {
        return ERROR;
    }

    psendmsg = (struct im_sendmsg_msgstruct *)calloc(1, sizeof(struct im_sendmsg_msgstruct));
    if (!psendmsg) 
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "calloc err");
        return ERROR;
    }
    root = cJSON_Parse(pmsg);
    if(root == NULL) 
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get root failed");
        return ERROR;
    }
    params = cJSON_GetObjectItem(root, "params");
    if((params == NULL))
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get params failed");
        goto OUT;
    }
    memset(&msg_head, 0, sizeof(msg_head));
    memset(&friend, 0, sizeof(friend));
    memset(&sendfile, 0, sizeof(sendfile));
    memset(&devinfo, 0, sizeof(devinfo));
    memset(psendmsg, 0, sizeof(struct im_sendmsg_msgstruct));
    msg_head.no_parse_msgid = 1;
    if (im_msghead_parses(root,params,&msg_head) != OK) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_msghead_parses failed");
        goto OUT;
    }
    
    switch(msg_head.im_cmdtype)
    {
        case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
            break;
            
        case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",friend.fromuser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",friend.nickname,PNR_USERNAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",friend.touser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendName",friend.friend_nickname,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserKey",friend.user_pubkey,PNR_USER_PUBKEY_MAXLEN);
            CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Result",friend.result,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterId",devinfo.user_devid, TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"RouterName",devinfo.user_devname, PNR_USERNAME_MAXLEN);

            //这里需要反向一下
            if(friend.result == OK)
            {
                pnr_friend_dbinsert(friend.touser_toxid,friend.fromuser_toxid,friend.nickname,friend.user_pubkey);
                im_nodelist_addfriend(id,friend.touser_toxid,friend.fromuser_toxid,friend.nickname,friend.user_pubkey);
                pnr_check_update_devinfo_bytoxid(index,friend.fromuser_toxid,devinfo.user_toxid,devinfo.user_devname);
            }
            break;
           
        case PNR_IM_CMDTYPE_DELFRIENDPUSH:
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",friend.fromuser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",friend.touser_toxid,TOX_ID_STR_LEN); 

			pnr_msgcache_dbdelete_by_friendid(id, friend.fromuser_toxid);
			im_nodelist_delfriend(id,friend.touser_toxid,friend.fromuser_toxid,1);
            pnr_friend_dbdelete(friend.touser_toxid,friend.fromuser_toxid,1);

			int friendnum = GetFriendNumInFriendlist_new(g_tox_linknode[id], friend.touser_toxid);
			if (friendnum >= 0) {
				tox_friend_delete(g_tox_linknode[id], friendnum, NULL);
			}
            break;
            
        case PNR_IM_CMDTYPE_PUSHMSG:
            if(msg_head.api_version == PNR_API_VERSION_V1)
            {
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FromId",psendmsg->fromuser_toxid,TOX_ID_STR_LEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"ToId",psendmsg->touser_toxid,TOX_ID_STR_LEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"SrcKey",psendmsg->msg_srckey,PNR_RSA_KEY_MAXLEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"DstKey",psendmsg->msg_dstkey,PNR_RSA_KEY_MAXLEN);
            }
            else if(msg_head.api_version == PNR_API_VERSION_V3)
            {
#if 0//暂时不用hashid
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"From",sendmsg.from_uid,PNR_USER_HASHID_MAXLEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"To",sendmsg.to_uid,PNR_USER_HASHID_MAXLEN);
                pnr_gettoxid_byhashid(sendmsg.from_uid,sendmsg.fromuser_toxid);
                pnr_gettoxid_byhashid(sendmsg.to_uid,sendmsg.touser_toxid);
#else
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"From",psendmsg->fromuser_toxid,TOX_ID_STR_LEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"To",psendmsg->touser_toxid,TOX_ID_STR_LEN);
#endif
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Sign",psendmsg->sign,PNR_RSA_KEY_MAXLEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Nonce",psendmsg->nonce,PNR_RSA_KEY_MAXLEN);
                CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"PriKey",psendmsg->prikey,PNR_RSA_KEY_MAXLEN);
            }
            CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgId",psendmsg->log_id,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Msg",psendmsg->msg_buff,IM_MSG_PAYLOAD_MAXLEN);
            index = get_indexbytoxid(psendmsg->touser_toxid);
            if(index)
            {
                pnr_msglog_getid(index, &msgid);
                if(msg_head.api_version == PNR_API_VERSION_V1)
                {
                    pnr_msglog_dbinsert_specifyid(index,PNR_IM_MSGTYPE_TEXT,msgid,psendmsg->log_id,MSG_STATUS_SENDOK,psendmsg->fromuser_toxid,
                        psendmsg->touser_toxid,psendmsg->msg_buff,psendmsg->msg_srckey,psendmsg->msg_dstkey,NULL,0);
                }
                else if(msg_head.api_version == PNR_API_VERSION_V3)
                {
                    pnr_msglog_dbinsert_specifyid_v3(index,PNR_IM_MSGTYPE_TEXT,msgid,psendmsg->log_id,MSG_STATUS_SENDOK,psendmsg->fromuser_toxid,
                        psendmsg->touser_toxid,psendmsg->msg_buff,psendmsg->sign,psendmsg->nonce,psendmsg->prikey,NULL,0);
#if 0//暂时不用hashid
                    int f_id = 0;
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"PushMsg:src hasdid(%s->%s)",sendmsg.from_uid,sendmsg.to_uid);
                    f_id = get_friendid_bytoxid(index,sendmsg.fromuser_toxid);
                    if(f_id >= 0 && f_id < PNR_IMUSER_FRIENDS_MAXNUM)
                    {
                        strcpy(sendmsg.from_uid,g_imusr_array.usrnode[index].friends[f_id].u_hashstr);
                        strcpy(sendmsg.to_uid,g_imusr_array.usrnode[index].u_hashstr);
                        DEBUG_PRINT(DEBUG_LEVEL_INFO,"PushMsg:renew hasdid(%s->%s)",sendmsg.from_uid,sendmsg.to_uid);
                    }
#endif
                }
                cJSON_ReplaceItemInObject(params,"MsgId",cJSON_CreateNumber(msgid));
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"imtox_pushmsg_predeal: ###renew msgid(%d)",msgid);
            }
            break;

        case PNR_IM_CMDTYPE_READMSGPUSH:
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",psendmsg->fromuser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",psendmsg->touser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"ReadMsgs",psendmsg->msg_buff,IM_MSG_MAXLEN);
            int tmp_msgid = 0;
            char* msgid_buff_end = NULL;
            char* tmp_msgid_head = NULL;
            index = get_indexbytoxid(psendmsg->touser_toxid);
            if(index)
            {
                msgid_buff_end = psendmsg->msg_buff + strlen(psendmsg->msg_buff);
                tmp_msgid_head = psendmsg->msg_buff;
                while(tmp_msgid_head != NULL)
                {
                    tmp_msgid = atoi(tmp_msgid_head);
                    if(tmp_msgid)
                    {
                        pnr_msglog_dbupdate_stauts_byid(index,tmp_msgid,MSG_STATUS_READ_OK);
                    }
                    tmp_msgid_head = strchr(tmp_msgid_head,',');
                    if(tmp_msgid_head)
                    {
                        tmp_msgid_head = tmp_msgid_head+1;
                        if(tmp_msgid_head >= msgid_buff_end)
                        {
                            break;
                        }
                    }
                    else
                    {
                        break;
                    }
                }
            }
            break;
        case PNR_IM_CMDTYPE_USERINFOPUSH:
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",friend.fromuser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",friend.touser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"NickName",friend.nickname,PNR_USERNAME_MAXLEN);
            im_update_friend_nickname(friend.fromuser_toxid,friend.touser_toxid,friend.nickname);
            break;           
        case PNR_IM_CMDTYPE_DELMSGPUSH:
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"UserId",psendmsg->fromuser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FriendId",psendmsg->touser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgId",psendmsg->log_id,TOX_ID_STR_LEN);
            //这里要修改推送到app的msgid
            index = get_indexbytoxid(psendmsg->touser_toxid);
            if(index)
            {
                pnr_msglog_dbget_dbid_bylogid(index,psendmsg->log_id,psendmsg->fromuser_toxid,psendmsg->touser_toxid,&msgid);
                cJSON_ReplaceItemInObject(params,"MsgId",cJSON_CreateNumber(msgid));
                //DEBUG_PRINT(DEBUG_LEVEL_INFO,"###renew msgid(%d)",msgid);
            }
            break;  
        case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
            break;

		case PNR_IM_CMDTYPE_PUSHFILE:
			CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FromId",sendfile.fromuser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"ToId",sendfile.touser_toxid,TOX_ID_STR_LEN);
            CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"MsgId",sendfile.log_id,0);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileName",sendfile.filename,UPLOAD_FILENAME_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileMD5",sendfile.md5,32);
            CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileSize",sendfile.filesize,0);
			CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileType",sendfile.msgtype,0);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"SrcKey",sendfile.srckey,PNR_RSA_KEY_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"DstKey",sendfile.dstkey,PNR_RSA_KEY_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FileInfo",fileinfo,PNR_FILEINFO_MAXLEN);
            CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"FilePath",filepath,UPLOAD_FILENAME_MAXLEN);
            //这里要修改推送到app的msgid
            index = get_indexbytoxid(sendfile.touser_toxid);
            if(index)
            {
                char fullfilename[512] = {0};
                p_realfilename = strrchr(filepath,'/');
                if(p_realfilename)
                {
                    p_realfilename++;
                }
                else
                {
                    p_realfilename = sendfile.filename;
                }
    			snprintf(fullfilename, sizeof(fullfilename), "%sr/%s", 
				    g_imusr_array.usrnode[index].userdata_pathurl,p_realfilename);
                if(strlen(fileinfo) > 0)
                {
                    strcat(fullfilename,",");
                    strcat(fullfilename,fileinfo);
                }
                pnr_msglog_getid(index, &msgid);
                pnr_msglog_dbinsert_specifyid(index,sendfile.msgtype,msgid,sendfile.log_id,MSG_STATUS_SENDOK,sendfile.fromuser_toxid,
                    sendfile.touser_toxid,sendfile.filename,sendfile.srckey,sendfile.dstkey,fullfilename,sendfile.filesize);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"pushmsg: renew msgid(%d)",msgid);
                cJSON_ReplaceItemInObject(params,"MsgId",cJSON_CreateNumber(msgid));
                //DEBUG_PRINT(DEBUG_LEVEL_INFO,"###renew msgid(%d)",msgid);
            }
            break;
            
        default:
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"bad cmd(%d)",msg_head.im_cmdtype);
            goto OUT;
    }
    
    pnr_msgcache_getid(id, &msgid);

    if (msg_head.im_cmdtype != PNR_IM_CMDTYPE_PUSHFILE) {
    	dup = cJSON_Duplicate(root, 1);
    	cJSON_AddItemToObject(dup, "msgid", cJSON_CreateNumber(msgid));
    	pmsg = cJSON_PrintUnformatted(dup);
    	cJSON_Delete(dup);
    }

    switch (msg_head.im_cmdtype) 
    {
        case PNR_IM_CMDTYPE_ADDFRIENDPUSH:
        case PNR_IM_CMDTYPE_ONLINESTATUSPUSH:
            pnr_msgcache_dbinsert(msgid, "", puser, msg_head.im_cmdtype, pmsg, 
                strlen(pmsg), NULL, NULL, 0, PNR_MSG_CACHE_TYPE_TOXA, 0, "","");
            free(pmsg);
            break;

    	case PNR_IM_CMDTYPE_DELFRIENDPUSH:
    		break;
    		
        case PNR_IM_CMDTYPE_ADDFRIENDREPLY:
        case PNR_IM_CMDTYPE_USERINFOPUSH:
            pnr_msgcache_dbinsert(msgid, friend.fromuser_toxid, friend.touser_toxid, 
                msg_head.im_cmdtype, pmsg, strlen(pmsg), NULL, NULL, 0, 
                PNR_MSG_CACHE_TYPE_TOXA, 0, "","");
            free(pmsg);
            break;
        case PNR_IM_CMDTYPE_PUSHMSG:
            if(msg_head.api_version == PNR_API_VERSION_V1)
            {
                pnr_msgcache_dbinsert(msgid, psendmsg->fromuser_toxid, psendmsg->touser_toxid, 
                    msg_head.im_cmdtype, pmsg, strlen(pmsg), NULL, NULL, psendmsg->log_id, 
                    PNR_MSG_CACHE_TYPE_TOXA, 0, psendmsg->msg_srckey,psendmsg->msg_dstkey);
            }
            else if(msg_head.api_version == PNR_API_VERSION_V3)
            {
                pnr_msgcache_dbinsert_v3(msgid, psendmsg->fromuser_toxid, psendmsg->touser_toxid, 
                    msg_head.im_cmdtype, pmsg, strlen(pmsg), NULL, NULL, psendmsg->log_id, 
                    PNR_MSG_CACHE_TYPE_TOXA, 0, psendmsg->sign,psendmsg->nonce,psendmsg->prikey);
            }
            free(pmsg);
            break;
        case PNR_IM_CMDTYPE_DELMSGPUSH:
        case PNR_IM_CMDTYPE_READMSGPUSH:
    		if (msg_head.im_cmdtype == PNR_IM_CMDTYPE_DELMSGPUSH) {
    			int userindex = get_indexbytoxid(psendmsg->touser_toxid);
    			pnr_msgcache_dbdelete_by_logid(userindex, psendmsg);
                pnr_msglog_dbdelete(userindex, 0, psendmsg->log_id, psendmsg->fromuser_toxid, psendmsg->touser_toxid);
            }
    		
            pnr_msgcache_dbinsert(msgid, psendmsg->fromuser_toxid, psendmsg->touser_toxid, 
                msg_head.im_cmdtype, pmsg, strlen(pmsg), NULL, NULL, psendmsg->log_id, 
                PNR_MSG_CACHE_TYPE_TOXA, 0, psendmsg->msg_srckey,psendmsg->msg_dstkey);
            free(pmsg);
            break;

        case PNR_IM_CMDTYPE_PUSHFILE:
            //新版逻辑名称
            if(p_realfilename)
            {            
                snprintf(filepath, UPLOAD_FILENAME_MAXLEN, "/user%d/r/%s", id, p_realfilename);
                snprintf(fullfile, UPLOAD_FILENAME_MAXLEN*2, "%sr/%s",
                    g_imusr_array.usrnode[id].userdata_pathurl, p_realfilename);
            }
            else
            {
                snprintf(filepath, UPLOAD_FILENAME_MAXLEN, "/user%d/r/%s", id, sendfile.filename);
                snprintf(fullfile, UPLOAD_FILENAME_MAXLEN*2, "%sr/%s",
                    g_imusr_array.usrnode[id].userdata_pathurl, sendfile.filename);
            }
            dup = cJSON_Duplicate(root, 1);
            params = cJSON_GetObjectItem(dup, "params");
            if (!params)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "get params err");
                cJSON_Delete(dup);
                break;
            }
            cJSON_AddItemToObject(dup, "msgid", cJSON_CreateNumber(msgid));
            cJSON_AddItemToObject(params, "FilePath", cJSON_CreateString(filepath));
            pmsg = cJSON_PrintUnformatted(dup);
            cJSON_Delete(dup);
            pnr_msgcache_dbinsert(msgid, sendfile.fromuser_toxid, sendfile.touser_toxid,
                msg_head.im_cmdtype, pmsg, strlen(pmsg), sendfile.filename, fullfile,
                sendfile.log_id, PNR_MSG_CACHE_TYPE_TOXA, sendfile.msgtype, sendfile.srckey,sendfile.dstkey);
            free(pmsg);
            break;  
        default:
            break;
    }

OUT:
    if(psendmsg)
    {
        free(psendmsg);
    }
	cJSON_Delete(root);
    return OK;
}

int im_server_init(void)
{
    int i = 0, j = 0;
    char cmd[CMD_MAXLEN] = {0};
    char user_rpath[PNR_FILEPATH_MAXLEN+1] = {0};
    char user_spath[PNR_FILEPATH_MAXLEN+1] = {0};
	char user_upath[PNR_FILEPATH_MAXLEN+1] = {0};
    memset(&g_daemon_tox,0,sizeof(g_daemon_tox));
    memset(&g_imusr_array,0,sizeof(g_imusr_array));

    if(access(PNR_AVATAR_FULLDIR,F_OK) != OK)
    {
        snprintf(cmd,CMD_MAXLEN,"mkdir -p %s",PNR_AVATAR_FULLDIR);
        system(cmd);   
    }
    if(access(PNR_FILECACHE_FULLDIR,F_OK) != OK)
    {
        snprintf(cmd,CMD_MAXLEN,"mkdir -p %s",PNR_FILECACHE_FULLDIR);
        system(cmd);   
    }
    //用户账号信息初始化
	pnr_account_init_fromdb();
	for(i=0;i<=PNR_IMUSER_MAXNUM;i++)
    {
        g_imusr_array.usrnode[i].user_index = i;
        snprintf(g_imusr_array.usrnode[i].user_name,PNR_USERNAME_MAXLEN,"user%d",i);
        strcpy(g_imusr_array.usrnode[i].userdata_pathurl,DAEMON_PNR_USERDATA_DIR);
        strcat(g_imusr_array.usrnode[i].userdata_pathurl,g_imusr_array.usrnode[i].user_name);
        strcat(g_imusr_array.usrnode[i].userdata_pathurl,"/");
        strcpy(g_imusr_array.usrnode[i].userinfo_pathurl,DAEMON_PNR_USERINFO_DIR);
        strcat(g_imusr_array.usrnode[i].userinfo_pathurl,g_imusr_array.usrnode[i].user_name);
        strcat(g_imusr_array.usrnode[i].userinfo_pathurl,"/");

		if(access(g_imusr_array.usrnode[i].userdata_pathurl,F_OK) != OK)
        {
            snprintf(cmd,CMD_MAXLEN,"mkdir -p %s",g_imusr_array.usrnode[i].userdata_pathurl);
            system(cmd);
		}
		if(access(g_imusr_array.usrnode[i].userinfo_pathurl,F_OK) != OK)
        {
            snprintf(cmd,CMD_MAXLEN,"mkdir -p %s",g_imusr_array.usrnode[i].userinfo_pathurl);
            system(cmd);
		}

		if (i > 0) {
            snprintf(user_spath,PNR_FILEPATH_MAXLEN,"%ss",g_imusr_array.usrnode[i].userdata_pathurl);
            snprintf(user_rpath,PNR_FILEPATH_MAXLEN,"%sr",g_imusr_array.usrnode[i].userdata_pathurl);
            snprintf(user_upath,PNR_FILEPATH_MAXLEN,"%su",g_imusr_array.usrnode[i].userdata_pathurl);

			if(access(user_spath,F_OK) != OK)
            {
     			snprintf(cmd,CMD_MAXLEN,"mkdir -p %s",user_spath);
			    system(cmd);   
            }

			if(access(user_rpath,F_OK) != OK)
            {
                snprintf(cmd,CMD_MAXLEN,"mkdir -p %s",user_rpath);
                system(cmd);   
            }

			if(access(user_upath,F_OK) != OK)
            {
                snprintf(cmd,CMD_MAXLEN,"mkdir -p %s",user_upath);
                system(cmd);   
            }
		}
		
        strcpy(g_imusr_array.usrnode[i].userinfo_fullurl,g_imusr_array.usrnode[i].userinfo_pathurl);
        strcat(g_imusr_array.usrnode[i].userinfo_fullurl,PNR_DATAFILE_DEFNAME);
        /*DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_server_init(%d): user(%s) datapath(%s) datafile(%s)",
            g_imusr_array.usrnode[i].user_index,g_imusr_array.usrnode[i].user_name,
            g_imusr_array.usrnode[i].userdata_pathurl,g_imusr_array.usrnode[i].userinfo_fullurl);*/

		//消息队列初始化
        INIT_LIST_HEAD(&g_lws_msglist[i].list);
        INIT_LIST_HEAD(&g_tox_msglist[i].list);
		INIT_LIST_HEAD(&g_lws_cache_msglist[i].list);

        pthread_mutex_init(&(lws_msglock[i]),NULL);
		pthread_mutex_init(&(lws_cache_msglock[i]),NULL);
        pthread_mutex_init(&(tox_msglock[i]),NULL);
        pthread_mutex_init(&(g_pnruser_lock[i]),NULL);
        pthread_mutex_init(&(g_user_friendlock[i]),NULL);
        pthread_mutex_init(&(g_user_msgidlock[i]),NULL);
        pthread_mutex_init(&(g_user_toxdatalock[i]),NULL);
        pthread_mutex_init(&(g_toxmsg_cache_lock[i]),NULL);
		//用户信息初始化,现在不用instance表了，直接在account表初始化的时候就赋值了
        //pnr_usr_instance_get(i);
        if(g_imusr_array.usrnode[i].user_toxid[0] != 0)
        {
            g_imusr_array.cur_user_num ++;
            pnr_uidhash_get(i,0,g_imusr_array.usrnode[i].user_toxid,
                &g_imusr_array.usrnode[i].hashid,g_imusr_array.usrnode[i].u_hashstr);
            /*DEBUG_PRINT(DEBUG_LEVEL_INFO,"id(%d) tox_id(%s) hashid(%d:%s)",
                i,g_imusr_array.usrnode[i].user_toxid,g_imusr_array.usrnode[i].hashid,g_imusr_array.usrnode[i].u_hashstr);*/
            //初始化好友列表
            pnr_dbget_friendsall_byuserid(i,g_imusr_array.usrnode[i].user_toxid);
            //初始化数据库句柄
            if (sql_msglogdb_init(i) != OK){
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msglog db failed", i);
                return ERROR;
            }
            if (sql_msgcachedb_init(i) != OK) {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "[%d]init msgcache db failed", i);
                return ERROR;
            }
        }

		for (j = 0; j <= PNR_IMUSER_FRIENDS_MAXNUM; j++) {
			pthread_mutex_init(&g_imusr_array.usrnode[i].friends[j].lock_sended, NULL);
		}
        //lws句柄指针
        g_lws_handler[i] = NULL;

        //tox linknode 指针
        g_tox_linknode[i] = NULL;
    }

    //用户消息缓存初始化
	pnr_msgcache_init();
    //用户tox data信息初始化
    pnr_tox_datafile_init_fromdb();
    //群信息初始化
    pnr_groupinfo_init();
    return OK;
}

#define IM_SERVER_LWS_PINGPONG		180
#define IM_SERVER_LWS_TIMEOUTMS		100 //100ms

int im_server_main(void)
{
	struct lws_context_creation_info info;
	struct lws_context *context;
	int n = 0, logs = LLL_INFO | LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;

	lws_set_log_level(logs, NULL);

	memset(&info, 0, sizeof info);
	info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info.options |= LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
	
	info.mounts = &mount;
	info.ws_ping_pong_interval = IM_SERVER_LWS_PINGPONG;
	info.ka_time = 10;
	info.ka_probes = 3;
	info.ka_interval = 3;
    info.gid = -1;
    info.uid = -1; 
	info.ssl_cert_filepath = WS_SERVER_SSLCERT_FILEPATH;
	info.ssl_private_key_filepath = WS_SERVER_PRIVATEKEY_FILEPATH;
	info.pt_serv_buf_size = LWS_MSGBUFF_MAXLEN;

	context = lws_create_context(&info);
	if (!context) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR,"lws init failed;exit");
		return 1;
	}

    info.port = PNR_IM_SERVER_PORT;
	info.protocols = protocols;
    info.vhost_name = "json server";
	if (!lws_create_vhost(context, &info)) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR,"lws init lws_create_vhost1 failed");
		goto fail;
	}

	info.port = PNR_IM_SERVER_PORT_BIN;
	info.protocols = bin_protocols;
	info.vhost_name = "binary server";
	if (!lws_create_vhost(context, &info)) {
		DEBUG_PRINT(DEBUG_LEVEL_ERROR,"lws init lws_create_vhost2 failed");
		goto fail;
	}

	while (n >= 0 && !interrupted)
		n = lws_service(context, IM_SERVER_LWS_TIMEOUTMS);

fail:
	lws_context_destroy(context);
	return 0;
}

/*****************************************************************************
 函 数 名  : imuser_friendstatus_push
 功能描述  : 好友状态更新推送
 输入参数  : int index          
             int online_status  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年10月10日
    作    者   : lichao
    修改内容   : 新生成函数

*****************************************************************************/
int imuser_friendstatus_push(int index,int online_status)
{
    int i=0,j=0,friend_id=0;
    struct im_friend_msgstruct user;

    //遗留问题，上线时如果好友不在线怎么处理？
    //对其所有的好友发送状态改变消息
    for(i=0;i<PNR_IMUSER_FRIENDS_MAXNUM;i++)
    {
       if(g_imusr_array.usrnode[index].friends[i].exsit_flag)
       {
           memset(&user,0,sizeof(user));
           memcpy(user.fromuser_toxid,g_imusr_array.usrnode[index].user_toxid,TOX_ID_STR_LEN);
           memcpy(user.touser_toxid,g_imusr_array.usrnode[index].friends[i].user_toxid,TOX_ID_STR_LEN);
           user.result = online_status;

           //先找到好友
           friend_id=get_indexbytoxid(g_imusr_array.usrnode[index].friends[i].user_toxid);
           //对于目标好友为本地用户
           if(friend_id != 0)
           {
                //再在好友的好友列表中找到自己，并且改变状态
                for(j=0;j<PNR_IMUSER_FRIENDS_MAXNUM;j++)
                {
                    if((g_imusr_array.usrnode[friend_id].friends[j].exsit_flag == TRUE)
                        &&(strcmp(g_imusr_array.usrnode[index].user_toxid,g_imusr_array.usrnode[friend_id].friends[j].user_toxid) == OK))
                    {
                        g_imusr_array.usrnode[friend_id].friends[j].online_status = online_status;
                        break;
                    }
                }
                //im_pushmsg_callback(friend_id,PNR_IM_CMDTYPE_ONLINESTATUSPUSH,TRUE,PNR_API_VERSION_V4,(void *)&user);
           }
           else
           {
           		//避免好友关系丢失导致无法接收消息
				check_and_add_friends(g_tox_linknode[index], g_imusr_array.usrnode[index].friends[i].user_toxid, 
					g_imusr_array.usrnode[index].userinfo_fullurl);
                //im_pushmsg_callback(index,PNR_IM_CMDTYPE_ONLINESTATUSPUSH,FALSE,PNR_API_VERSION_V4,(void *)&user);
           }
       }
    }
    return OK;
}

/**********************************************************************************
  Function:      imuser_heartbeat_deal
  Description:   心跳处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/   
int imuser_heartbeat_deal(void)
{
    int index = 0;

    //遍历，对所有非离线状态的用户检测心跳计数
    for(index=1;index<=PNR_IMUSER_MAXNUM;index++)
    {
        pthread_mutex_lock(&(g_pnruser_lock[index]));
        if(g_imusr_array.usrnode[index].user_onlinestatus != USER_ONLINE_STATUS_OFFLINE)
        {
            g_imusr_array.usrnode[index].heartbeat_count ++;
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d %s) count++",index,g_imusr_array.usrnode[index].user_toxid);
            if(g_imusr_array.usrnode[index].heartbeat_count >= IMUSER_HEARTBEAT_OFFLINENUM)
            {
                g_imusr_array.usrnode[index].user_onlinestatus = USER_ONLINE_STATUS_OFFLINE;
                g_imusr_array.usrnode[index].user_online_type = USER_ONLINE_TYPE_NONE;
                g_imusr_array.usrnode[index].notice_flag = FALSE;
                imuser_friendstatus_push(index,USER_ONLINE_STATUS_OFFLINE);
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d %s) offline",index,g_imusr_array.usrnode[index].user_toxid);
            }
        }
        pthread_mutex_unlock(&(g_pnruser_lock[index]));
    }
    return OK;
}
/**********************************************************************************
  Function:      im_global_info_show
  Description:   全局调试信息
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_global_info_show(char* pcmd)
{
    FILE * fp = NULL;
    int i = 0,usernum = 0, j = 0;
    char* pbuf = NULL;
    int show_type = 0;
    //char qrcode_buf[PNR_QRCODE_SRCLEN*3+1] = {0};
    int ret_len =0;
    struct stroage_info_struct stroage_info;
    struct im_user_struct* puser = NULL;
    struct pnr_account_struct account;
    char nickname[PNR_USERNAME_MAXLEN+1] = {0};
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_global_info_show in");

    show_type = atoi(pcmd);
    switch(show_type) 
    {
        case PNR_GLOBAL_SHOWINFO_USERLIST:
            unlink(PNR_DEBUG_FILENAME);
        	fp = fopen(PNR_DEBUG_FILENAME,"w+");
            if(fp == NULL)
        	{
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR,"file(%s) fopen failed",PNR_DEBUG_FILENAME);
        		return ERROR;
        	}
        	fprintf(fp,"\\*******************************USER INFO**********************\\\n");
            for(i=1;i<=PNR_IMUSER_MAXNUM;i++)
            {
                puser = &g_imusr_array.usrnode[i];
                if(puser->user_toxid[0] != 0)
                {
                    fprintf(fp,"\\id(%d):tox(%s) init(%d) online(%d) con_type(%d)\\\n",i,puser->user_toxid,
                        puser->init_flag,puser->user_onlinestatus,puser->user_online_type);
                    usernum++;
                    for(j=0;j<puser->friendnum;j++)
                    {
                        if(puser->friends[j].exsit_flag)
                        {
                            fprintf(fp,"\\----friend(%d):tox(%s) name(%s) online(%d)\\\n",(j+1),
                                puser->friends[j].user_toxid,puser->friends[j].user_nickname,puser->friends[j].online_status);
                        }
                    }
                }
            }
            fprintf(fp,"\\current user num %d\\\n",usernum);
        	fclose(fp);
            break;
        case PNR_GLOBAL_SHOWINFO_STATUS:
            unlink(PNR_STATUS_FILENAME);
        	fp = fopen(PNR_STATUS_FILENAME,"w+");
            if(fp == NULL)
        	{
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR,"file(%s) fopen failed",PNR_STATUS_FILENAME);
        		return ERROR;
        	}
            //pnr_create_account_qrcode(g_account_array.admin_account.user_sn,qrcode_buf,&ret_len);
            //fprintf(fp,"%s %s\n",PNR_ADMINUSER_SN,qrcode_buf);
            fprintf(fp,"%s %d\n",PNR_CUR_NORMALUSER_NUM,g_account_array.normal_user_num);
            fprintf(fp,"%s %d\n",PNR_CUR_TEMPUSER_NUM,g_account_array.temp_user_num);
            memset(&stroage_info,0,sizeof(stroage_info));
            get_storageinfo(&stroage_info);
            fprintf(fp,"%s %sB\n",PNR_TOTAL_STORAGE_SPACE,stroage_info.total_info_string);
            fprintf(fp,"%s %sB\n",PNR_FREE_STORAGE_SPACE,stroage_info.free_info_string);
            fclose(fp);
            break;
        case PNR_SHOWINFO_CHECKUSER_BYTOXID:
            pbuf = strchr(pcmd,' ');
            if(pbuf == NULL)
            {
                return ERROR;
            }
            pbuf++;
            while(pbuf[0] == ' ' || pbuf[0] == '\t')
            {
                pbuf++;
            }
            if(strlen(pbuf) != TOX_ID_STR_LEN)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_global_info_show:bad toxid(%s)",pbuf);
                return ERROR;
            }
            i = get_indexbytoxid(pbuf);
            if(i == 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_global_info_show:not found toxid(%s)",pbuf);
                return ERROR;
            }
            memset(&account,0,sizeof(account));
            strcpy(account.toxid,pbuf);
            pnr_account_dbget_byuserid(&account);
            unlink(PNR_STATUS_FILENAME);
        	fp = fopen(PNR_STATUS_FILENAME,"w+");
            if(fp == NULL)
        	{
        		DEBUG_PRINT(DEBUG_LEVEL_ERROR,"file(%s) fopen failed",PNR_STATUS_FILENAME);
        		return ERROR;
        	}
            fprintf(fp,"user(%d)\ttoxid(%s)\n",i,g_imusr_array.usrnode[i].user_toxid);
            fprintf(fp,"online_status(%d)\tconn_status(%d)\n",
                g_imusr_array.usrnode[i].user_onlinestatus,g_imusr_array.usrnode[i].user_online_type);
            fprintf(fp,"user active(%d)\tsn(%s)\t",account.active,account.user_sn);
            if(account.active == TRUE)
            {
                pnr_base64_decode(account.nickname,strlen(account.nickname),nickname,&ret_len);
                fprintf(fp,"nickname(%s)\n",nickname);
                fprintf(fp,"#################friends list#################\n");
                for(j=0;j<PNR_IMUSER_FRIENDS_MAXNUM;j++)
                {
                    if(g_imusr_array.usrnode[i].friends[j].exsit_flag == TRUE)
                    {
                        memset(nickname,0,PNR_USERNAME_MAXLEN);
                        pnr_base64_decode(g_imusr_array.usrnode[i].friends[j].user_nickname,strlen(account.nickname),nickname,&ret_len);
                        fprintf(fp,"friend(%d)\tfriend_toxid(%s)\tfriend_nickname(%s)\n",
                            j,g_imusr_array.usrnode[i].friends[j].user_toxid,nickname);
                    }
                }
            }
            fprintf(fp,"###################################################\n");
            fclose(fp);
            break;
        case PNR_SHOWINFO_CHECKUSER_USERINDEX:
            pbuf = strchr(pcmd,' ');
            if(pbuf == NULL)
            {
                return ERROR;
            }
            pbuf++;
            while(pbuf[0] == ' ' || pbuf[0] == '\t')
            {
                pbuf++;
            }
            i = atoi(pbuf);
            if(i < 0 || i > PNR_IMUSER_MAXNUM)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_global_info_show:bad index(%s)",pbuf);
                return ERROR;
            }
            memset(&account,0,sizeof(account));
            strcpy(account.toxid,g_imusr_array.usrnode[i].user_toxid);
            pnr_account_dbget_byuserid(&account);
            unlink(PNR_STATUS_FILENAME);
            fp = fopen(PNR_STATUS_FILENAME,"w+");
            if(fp == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"file(%s) fopen failed",PNR_STATUS_FILENAME);
                return ERROR;
            }
            fprintf(fp,"user(%d)\ttoxid(%s)\n",i,g_imusr_array.usrnode[i].user_toxid);
            fprintf(fp,"online_status(%d)\tconn_status(%d)\n",
                g_imusr_array.usrnode[i].user_onlinestatus,g_imusr_array.usrnode[i].user_online_type);
            fprintf(fp,"user active(%d)\tsn(%s)\t",account.active,account.user_sn);
            if(account.active == TRUE)
            {
                pnr_base64_decode(account.nickname,strlen(account.nickname),nickname,&ret_len);
                fprintf(fp,"nickname(%s)\n",nickname);
                fprintf(fp,"#################friends list#################\n");
                for(j=0;j<PNR_IMUSER_FRIENDS_MAXNUM;j++)
                {
                    if(g_imusr_array.usrnode[i].friends[j].exsit_flag == TRUE)
                    {
                        memset(nickname,0,PNR_USERNAME_MAXLEN);
                        pnr_base64_decode(g_imusr_array.usrnode[i].friends[j].user_nickname,strlen(account.nickname),nickname,&ret_len);
                        fprintf(fp,"friend(%d)\tfriend_toxid(%s)\tfriend_nickname(%s)\n",
                            j,g_imusr_array.usrnode[i].friends[j].user_toxid,nickname);
                    }
                }
            }
            fprintf(fp,"###################################################\n");
            fclose(fp);
            break;
        default:
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_global_info_show:bad cmd(%d)",show_type);
            break;
    }       
    return OK;
}
/**********************************************************************************
  Function:      pnr_datafile_base64encode
  Description:   data文件base64编码
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int pnr_datafile_base64encode(char* file_url,char* encode_buff,int* encode_buflen)
{
    char data_buff[DATAFILE_BUFF_MAXLEN+1] = {0};
    FILE *fp = NULL;
    int data_srclen = 0;
    char* fpos = data_buff;
    if(file_url == NULL || encode_buff == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64encode:input error");
        return ERROR;
    }
	fp = fopen (file_url, "rb");
    if(fp == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64encode:open file(%s) error",file_url);
        return ERROR;
    }
    while(!feof(fp))/*判断是否结束，这里会有个文件结束符*/
    {
        fread(fpos,sizeof(char),1,fp);
        data_srclen++;
        fpos++;
    }   
    fclose(fp);

    if(pnr_base64_encode(data_buff,data_srclen-1,encode_buff,encode_buflen) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64encode:pnr_base64_encode error");
        return ERROR;
    }
    /*DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64encode:file(%s) len (%d) encode_result(%d:%s)",
        file_url,data_srclen,*encode_buflen,encode_buff);*/
    return OK;
}
/**********************************************************************************
  Function:      pnr_datafile_base64decode
  Description:   base64字符串解码为data文件
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int pnr_datafile_base64decode(char* file_url,char* src_buff,int src_buflen)
{
    char data_buff[DATAFILE_BUFF_MAXLEN+1] = {0};
    FILE *fp = NULL;
    int data_len = 0;
    int retlen = 0;
    if(file_url == NULL || src_buff == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64decode:input error");
        return ERROR;
    }

    if(pnr_base64_decode(src_buff,src_buflen,data_buff,&data_len) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64decode:pnr_base64_encode error");
        return ERROR;
    }
    if(data_len <= 0)
    {        
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64decode:pnr_base64_encode error2");
        return ERROR;
    }

    /*DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64decode:file(%s) len (%d) encode_result(%d:%s)",
        file_url,data_len,src_buflen,src_buff);*/

	fp = fopen (file_url, "wb");
    if(fp == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64decode:open file(%s) error",file_url);
        return ERROR;
    }
    retlen = fwrite(data_buff,1,data_len,fp); 
    fclose(fp);
    if(retlen != data_len)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_datafile_base64decode write file(%s) len(%d)",file_url,retlen);
    }
    return OK;
}
/**********************************************************************************
  Function:      im_datafile_base64_change
  Description:   data文件格式转换
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_datafile_base64_change(char* src_file,char* dst_file,int encode_flag)
{
    char base64_buf[DATAFILE_BASE64_ENCODE_MAXLEN+1] = {0};
    int bufflen = 0;
    int retlen = 0;
    char* fpos = base64_buf;
    FILE *fp = NULL;
    if(src_file == NULL || dst_file == NULL)
    {
        return ERROR;
    }
    //如果是true，base64加密
    if(encode_flag == TRUE)
    {
        if(pnr_datafile_base64encode(src_file,base64_buf,&bufflen) != OK)
        {
            return ERROR;
        }
    	fp = fopen (dst_file, "wb");
        if(fp == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_datafile_base64_change:open file(%s) error",dst_file);
            return ERROR;
        }
        retlen = fwrite(base64_buf,1,bufflen,fp);
        fclose(fp);        
        if(retlen != bufflen)
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_datafile_base64_change:write len(%d)",retlen);
        }
    }
    else
    {
        fp = fopen (src_file, "rb");
        if(fp == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_datafile_base64_change:open file(%s) error",src_file);
            return ERROR;
        }
        while(!feof(fp))/*判断是否结束，这里会有个文件结束符*/
        {
            fread(fpos,sizeof(char),1,fp);
            bufflen++;
            fpos++;
        }   
        fclose(fp);
        //DEBUG_PRINT(DEBUG_LEVEL_INFO,"decode(%d:%s)",bufflen,base64_buf);
        bufflen--;//去掉最后的结尾符
        if(pnr_datafile_base64decode(dst_file,base64_buf,bufflen) != OK)
        {
            return ERROR;
        }
    }
    return OK;
}
/**********************************************************************************
  Function:      im_datafile_base64_change_cmddeal
  Description:   data文件格式转换命令处理
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_datafile_base64_change_cmddeal(char* pcmd)
{
    char src_file[PNR_FILEPATH_MAXLEN+1] = {0};
    char dst_file[PNR_FILEPATH_MAXLEN+1] = {0};
    int encode_flag = 0;
    if(pcmd == NULL)
    {
        return ERROR;
    }
	sscanf(pcmd, "%d %s %s",&encode_flag,src_file,dst_file);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_datafile_base64_change_cmddeal(%d:%s->%s)",
        encode_flag,src_file,dst_file);
    return im_datafile_base64_change(src_file,dst_file,encode_flag);
}

/**********************************************************************************
  Function:      im_delete_friend_filelog
  Description:   删除好友预处理，删除好友之间文件记录
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_delete_friend_filelog(int msgid,int filetype,char* user_id,char* friend_id)
{
    return OK;
}
/**********************************************************************************
  Function:      im_delete_friend_predeal
  Description:   删除好友预处理，删除好友之间对应的聊天记录，文件传输记录
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_delete_friend_predeal(char* user_id,char* friend_id)
{
    int user_index = 0;
    if(user_id == NULL || friend_id == NULL) 
    {
        return ERROR;
    }
    user_index = get_indexbytoxid(user_id);
    if(user_index == 0)
    {
        return ERROR;
    }
    //聊天记录直接删除
    pnr_msglog_dbdelete(user_index,PNR_IM_MSGTYPE_TEXT,0,user_id,friend_id);
    //音频视频文件传输，先根据消息记录，清理对应文件，再删除消息记录
    return OK;
}
/**********************************************************************************
  Function:      pnr_qrcode_encrype
  Description:  二维码加密
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int pnr_qrcode_encrype(char* src_msg,char* p_ret,int* ret_len)
{
    char qrcode_buf[PNR_QRCODE_MAXLEN+1]={0};
    char aes_result[PNR_QRCODE_SRCLEN*3+1] = {0};
    unsigned int keyword[60];
    int srclen = 0;
    int padlen = 0, i = 0;
    char padpay = 0;
    int ret = 0;
    if(src_msg == NULL)
    {
        return ERROR;
    }
    strcpy(qrcode_buf,src_msg);        
    srclen = strlen(qrcode_buf);
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_create_account_qrcode:qrcode_buf(%s),len(%d)",qrcode_buf,srclen);
    if((srclen%AES_BLOCK_SIZE) != 0)
    {
        padlen = srclen;
        padpay = AES_BLOCK_SIZE - (padlen%AES_BLOCK_SIZE);
        srclen = ((srclen/AES_BLOCK_SIZE)+1)*AES_BLOCK_SIZE;
        for(i=padlen; i<srclen; i++)
        {
            qrcode_buf[i] = padpay;
            //DEBUG_PRINT(DEBUG_LEVEL_INFO,"get i(%d) padpay(%d)",i,padpay);
        }
    }
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_create_account_qrcode:qrcode_buf(%s),len(%d)",qrcode_buf,srclen);
    aes_key_setup(PNR_USN_KEY_V101_WORD, keyword, PNR_AES_CBC_KEYSIZE);
    ret = aes_encrypt_cbc((unsigned char*)qrcode_buf,srclen,(unsigned char*)aes_result,keyword,PNR_AES_CBC_KEYSIZE,PNR_USN_IVKEY_V101_WORD);
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_create_account_qrcode:aes_result(%d:%s)",ret,aes_result);
    pnr_base64_encode(aes_result,srclen,p_ret,ret_len);
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_create_account_qrcode:get(%d:%s)",*ret_len,p_ret);
    return ret;
}

/**********************************************************************************
  Function:      pnr_create_account_qrcode
  Description:  生成新的账户二维码
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:调用成功
                 1:调用失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/
int pnr_create_account_qrcode(char* p_usn,char* p_ret,int* ret_len)
{
    char qrcode_buf[PNR_QRCODE_MAXLEN+1]={0};
    int encrype_len = 0;

    if(p_ret == NULL || p_usn == NULL)
    {
        return ERROR;
    }
    strcpy(qrcode_buf,PNR_USN_KEY_VERSION_STR);
    strcat(qrcode_buf,g_daemon_tox.user_toxid);
    strcat(qrcode_buf,p_usn);
    strcpy(p_ret,g_qrcode_head[PNR_QRCODE_TYPE_ACCOUNTINFO]);
    if(pnr_qrcode_encrype(qrcode_buf,p_ret+PNR_QRCODE_HEADOFFSET,&encrype_len) != OK)
    {
        return ERROR;
    }
    *ret_len = encrype_len+PNR_QRCODE_HEADOFFSET;
    return OK;
}
/**********************************************************************************
  Function:      im_account_qrcode_get_cmddeal
  Description:   获取账号二维码的命令
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_account_qrcode_get_cmddeal(char* pcmd)
{
    char user_sn[PNR_USN_MAXLEN+1] = {0};
    char qrcode_buf[PNR_QRCODE_SRCLEN*3+1] = {0};
    int ret_len =0;
    int user_type = 0;
    int user_index = 0;
    char* ptmp = NULL;
    if(pcmd == NULL)
    {
        return ERROR;
    }
    user_type = atoi(pcmd);
    switch(user_type)
    {
        case PNR_USER_TYPE_ADMIN:
            strcpy(user_sn,g_account_array.account[PNR_ADMINUSER_PSN_INDEX].user_sn);
            break;
        case PNR_USER_TYPE_NORMAL:
            ptmp = strchr(pcmd,0x20);
            if(ptmp != NULL)
            {
                ptmp ++;
                user_index = atoi(ptmp);
            }
            if(user_index < 0 || user_index > PNR_IMUSER_MAXNUM)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_account_qrcode_get_cmddeal:bad parms(%s)",pcmd);
                return ERROR;
            }
            else if(user_index == 0)
            {
                user_index= g_account_array.total_user_num+1;
            }
            pnr_create_usersn(PNR_USER_TYPE_NORMAL,user_index,user_sn);
            break;
        case PNR_USER_TYPE_TEMP:
            strcpy(user_sn,g_account_array.temp_user_sn);
            break;
    }
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"get user_type(%d) user_sn(%s)",user_type,user_sn);
    pnr_create_account_qrcode(user_sn,qrcode_buf,&ret_len);    
    return OK;
}
/**********************************************************************************
  Function:      im_debug_imcmd_deal
  Description:   测试指令
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_debug_imcmd_deal(char* pcmd)
{
    char friend_id[TOX_ID_STR_LEN+1] = {0};
    int cmd_type = 0;
    int user_index = 0;
    char* pbuf = NULL;
    int run = 0;
    int toxfriend_id = 0;
    char* pmsg = NULL;
    int msglen = 0;
    int msgid = 0;
    struct pnr_account_struct account;

    if(pcmd == NULL)
    {
        return ERROR;
    }
    cmd_type = atoi(pcmd);
    pbuf = strchr(pcmd,' ');
    if(pbuf == NULL)
    {
        return ERROR;
    }
    pbuf++;
    user_index = atoi(pbuf);
    if(user_index <= 0 || user_index > PNR_IMUSER_MAXNUM)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_debug_imcmd_deal bad userindex(%d)",user_index);
        return ERROR;
    }
    switch(cmd_type)
    {      
        // 1 初始化一个实例
        case PNR_IM_CMDTYPE_LOGIN:
            if(g_imusr_array.usrnode[user_index].init_flag == TRUE)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%d:%s) already login",user_index,g_imusr_array.usrnode[user_index].user_toxid);
                break;
            }
            if (pthread_create(&g_imusr_array.usrnode[user_index].tox_tid, NULL, imstance_daemon, &user_index) != 0) 
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"create tox_instance failed");
            }  
            else
            {
                while(g_imusr_array.usrnode[user_index].init_flag != TRUE && run < 5)
                {
                    sleep(1);
                    run++;
                }
                if(run >= 5)
                {
                    return ERROR;
                }
                else
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d:%s) login",user_index,g_imusr_array.usrnode[user_index].user_toxid);
                }
            }
            break;
            // 2 销毁一个实例
        case PNR_IM_CMDTYPE_DESTORY:
            if(g_imusr_array.usrnode[user_index].init_flag == TRUE)
            {
                pthread_cancel(g_imusr_array.usrnode[user_index].tox_tid);
                g_imusr_array.usrnode[user_index].init_flag = FALSE;
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d:%s) logout",user_index,g_imusr_array.usrnode[user_index].user_toxid);
            }
            break;
            // 3.添加好友
        case PNR_IM_CMDTYPE_ADDFRIENDREQ:
            pbuf = strchr(pbuf,' ');
            if(pbuf == NULL)
            {
                return ERROR;
            }
            pbuf++;
            strncpy(friend_id,pbuf,TOX_ID_STR_LEN);
            if(g_imusr_array.usrnode[user_index].init_flag == FALSE)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%d:%s) not up",user_index,g_imusr_array.usrnode[user_index].user_toxid);
                return ERROR;
            }
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d:%s) tryto add friend(%s)",user_index,
                g_imusr_array.usrnode[user_index].user_toxid,friend_id);
            toxfriend_id = check_and_add_friends(g_imusr_array.usrnode[user_index].ptox_handle,
                friend_id,g_imusr_array.usrnode[user_index].userinfo_fullurl);
            if (toxfriend_id < 0) 
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"add fail");
                return ERROR;
            }
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"add success");
            break;
            // 7 删除好友
        case PNR_IM_CMDTYPE_DELFRIENDCMD:
            pbuf = strchr(pbuf,' ');
            if(pbuf == NULL)
            {
               return ERROR;
            }
            pbuf++;
            strncpy(friend_id,pbuf,TOX_ID_STR_LEN);
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d:%s) tryto del friend(%s)",user_index,
                g_imusr_array.usrnode[user_index].user_toxid,friend_id);
            if(g_imusr_array.usrnode[user_index].init_flag == FALSE)
            {
               DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%d:%s) not up",user_index,g_imusr_array.usrnode[user_index].user_toxid);
               return ERROR;
            }
            toxfriend_id = GetFriendNumInFriendlist_new(g_imusr_array.usrnode[user_index].ptox_handle,friend_id);
		    if (toxfriend_id < 0) 
            {    
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get friend_id fail");
                return ERROR;
            }
            Tox_Err_Friend_Delete error;
            if(tox_friend_delete(g_imusr_array.usrnode[user_index].ptox_handle,toxfriend_id,&error) == 0)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"del fail");
                return ERROR;
            }
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"del success");
            break;
            //9 发送消息
        case PNR_IM_CMDTYPE_SENDMSG:
            pbuf = strchr(pbuf,' ');
            if(pbuf == NULL)
            {
               return ERROR;
            }
            pbuf++;
            strncpy(friend_id,pbuf,TOX_ID_STR_LEN);
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"user(%d:%s) tryto sendmsg friend(%s)",user_index,
                g_imusr_array.usrnode[user_index].user_toxid,friend_id);
            if(g_imusr_array.usrnode[user_index].init_flag == FALSE)
            {
               DEBUG_PRINT(DEBUG_LEVEL_ERROR,"user(%d:%s) not up",user_index,g_imusr_array.usrnode[user_index].user_toxid);
               return ERROR;
            }
            toxfriend_id = GetFriendNumInFriendlist_new(g_imusr_array.usrnode[user_index].ptox_handle,friend_id);
		    if (toxfriend_id < 0) 
            {    
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"get friend_id fail");
                return ERROR;
            }
            pmsg = strchr(pbuf,' ');
            if(pmsg == NULL)
            {
               return ERROR;
            }
            pmsg++;
            msglen = strlen(pmsg);
            msgid = tox_friend_send_message(g_imusr_array.usrnode[user_index].ptox_handle, toxfriend_id, 
                TOX_MESSAGE_TYPE_NORMAL,(uint8_t *)pmsg, msglen, NULL);
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"send msg(%s) return %d",pmsg,msgid);
            break;
        //新生成一个普通用户
        case PNR_IM_CMDTYPE_CREATENORMALUSER:
            account.type = user_index;
            account.index = g_account_array.total_user_num+1;//这里要加上默认的admin用户的一个数量
            g_account_array.normal_user_num++;
            g_account_array.total_user_num++;
            if(account.type == PNR_USER_TYPE_ADMIN)
            {
                strcpy(account.mnemonic,PNR_ADMINUSER_MNEMONIC);
                strcpy(account.identifycode,"12345678"); 
            }
            else if(account.type == PNR_USER_TYPE_NORMAL)
            {
                strcpy(account.mnemonic,PNR_NORMALUSER_MNEMONIC);
                strcpy(account.identifycode,"11111111"); 
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad account type(%d)",account.type);
                return ERROR;
            }
            account.active = FALSE;
            pnr_create_usersn(account.type,g_account_array.normal_user_num,account.user_sn);
            pnr_account_dbinsert(&account);
            memcpy(&g_account_array.account[account.index],&account,sizeof(account));
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"create new account(%d:%s)",account.index,account.user_sn);
            //pnr_create_account_qrcode(account.user_sn,qrcode_buf,&ret_len);    
            break;
        default:
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_debug_imcmd_deal:bad cmd(%d)",cmd_type);
            break;
    }  
    return OK;
}
/**********************************************************************************
  Function:      adminaccount_qrcode_init
  Description:   adminuser 二维码文件生成
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int adminaccount_qrcode_init(void)
{
    int ret_len =0;
    //tmp账户生成二维码
    pnr_create_account_qrcode(g_account_array.temp_user_sn,g_account_array.temp_user_qrcode,&ret_len);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"adminaccount_qrcode_init:temp usn(%s) qrcode(%s)",g_account_array.temp_user_sn,g_account_array.temp_user_qrcode);
    //主派生账户写二维码文件
    pnr_create_account_qrcode(g_account_array.account[PNR_ADMINUSER_PSN_INDEX].user_sn,g_account_array.defadmin_user_qrcode,&ret_len);
#ifdef DEV_ONESPACE
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"adminaccount_qrcode_init:get def adminuser qrcode(%s)",g_account_array.defadmin_user_qrcode);
#else
    pnr_qrcode_create_png_bycmd(g_account_array.defadmin_user_qrcode,PNR_ADMINUSER_QRCODEFILE);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"adminaccount_qrcode_init:qrcode(%s) to file(%s)",g_account_array.defadmin_user_qrcode,PNR_ADMINUSER_QRCODEFILE);
#endif
    return OK;
}
/**********************************************************************************
  Function:      adminaccount_qrcode_show
  Description:   adminuser 二维码文件显示在shell上
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int account_qrcode_show(char* p_sn)
{
    char qrcode_buf[PNR_QRCODE_SRCLEN*3+1] = {0};
    int ret_len =0;
    char cmd[CMD_MAXLEN] = {0};
    char recv[CMD_MAXLEN] = {0};
    FILE *fp = NULL;
    if(p_sn == NULL)
    {
        return ERROR;
    }
    snprintf(cmd,CMD_MAXLEN,"cat %s",PNR_P2PID_FILE);
    if (!(fp = popen(cmd, "r"))) 
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"popen cmd(%s) failed",cmd);
        return ERROR;
    }
    if (fgets(recv,CMD_MAXLEN,fp) <= 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"failed cmd =%s",cmd);
        pclose(fp);
        return ERROR;
    }  
    pclose(fp);
    strncpy(g_daemon_tox.user_toxid,recv,TOX_ID_STR_LEN);
    pnr_create_account_qrcode(p_sn,qrcode_buf,&ret_len);
    pnr_qrcode_create_utf8(qrcode_buf,NULL);
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"adminaccount_qrcode_init ok");
    return OK;
}
/**********************************************************************************
  Function:      pnr_encrypt_show
  Description:   测试加密接口
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int pnr_encrypt_show(char* msg,int flag)
{
    char ret_buf[PNR_QRCODE_SRCLEN*3+1] = {0};
    int ret_len =0;
    int offset = PNR_QRCODE_HEADOFFSET;
    strcpy(ret_buf,g_qrcode_head[PNR_QRCODE_TYPE_DEVINFO]);
    pnr_qrcode_encrype(msg,ret_buf+offset,&ret_len);
    if(flag == TRUE)
    {
        printf("msg(%s)\nencrypt(%s)\n",msg,ret_buf);        
    }
    return OK;
}
int g_post_usernum = 0;
char g_post_userstring[PNR_POST_USERSTR_MAXLEN+1] = {0};
pthread_mutex_t g_postlock = PTHREAD_MUTEX_INITIALIZER;
int g_devreg_repeat_time = PNR_REPEAT_TIME_15MIN;
int g_udpdebug_flag = FALSE;
/**********************************************************************************
  Function:      pnr_post_attach_touser
  Description:   post_newmsg用户添加
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int pnr_post_attach_touser(char* uid)
{
    if(uid == NULL || strlen(uid)!= TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad uid(%s)",uid);
        return ERROR;
    }
    else
    {
        pthread_mutex_lock(&g_postlock);
        if(g_post_usernum >= 0 && g_post_usernum < PNR_POST_USER_MAXNUM)
        {
            if(g_post_usernum == 0)
            {
                strcpy(g_post_userstring,uid);
            }
            else
            {
                strcat(g_post_userstring,",");
                strcat(g_post_userstring,uid);
            }
            g_post_usernum++;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"post attach(%d:%s)",g_post_usernum,uid);
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad postnum(%d)",g_post_usernum);
        }
        pthread_mutex_unlock(&g_postlock);
    }
    return OK;
}
/**********************************************************************************
  Function:      pnr_post_newmsg_notice_task
  Description:   新消息提醒推送任务
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
static void *pnr_post_newmsg_notice_task(void *para)
{
    struct newmsg_notice_params* pmsg = (struct newmsg_notice_params*)para;
    char* post_data = NULL;
    char* rec_buff = NULL;
    int data_len = 0;
    cJSON * params = NULL;
    char server_url[URL_MAX_LEN] = {0};
    if(pmsg == NULL)
    {
        return NULL;
    }
    if(pmsg->server_flag == FALSE)
    {
        strcpy(server_url,PAPUSHMSG_DEVELOP_HTTPS_SERVER);
    }
    else
    {
        strcpy(server_url,PAPUSHMSG_PRODUCT_HTTPS_SERVER);
    }
    rec_buff = malloc(BUF_LINE_MAX_LEN+1);
    if(rec_buff == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"rec_buff malloc err");
        free(pmsg);
        return NULL;
    }
    memset(rec_buff,0,BUF_LINE_MAX_LEN);
    params = cJSON_CreateObject();
    if(params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"json create err");
        free(pmsg);
        free(rec_buff);
        return NULL;
    }
    cJSON_AddItemToObject(params, "from", cJSON_CreateString(pmsg->from));
    cJSON_AddItemToObject(params, "to", cJSON_CreateString(pmsg->to));
    cJSON_AddItemToObject(params, "title", cJSON_CreateString(pmsg->title));
    cJSON_AddItemToObject(params, "dev", cJSON_CreateString(pmsg->dev));
    cJSON_AddItemToObject(params, "payload", cJSON_CreateString(pmsg->payload));
    cJSON_AddItemToObject(params, "priority", cJSON_CreateNumber(pmsg->priority));
    cJSON_AddItemToObject(params, "type", cJSON_CreateNumber(pmsg->type));
    post_data = cJSON_PrintUnformatted_noescape(params);
    cJSON_Delete(params);
    data_len = strlen(post_data);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_post_newmsg_notice_task:post_data(%s)",post_data);
    https_post(server_url,PAPUSHMSG_HTTPSSERVER_PORT,PAPUSHMSG_HTTPSSERVER_PREURL,post_data,data_len,rec_buff,BUF_LINE_MAX_LEN);
    free(pmsg);
    free(post_data);
    params = cJSON_CreateObject();
    if(params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"json create err");
        free(rec_buff);
        return NULL;
    }
    cJSON_AddItemToObject(params, "replay", cJSON_CreateString(rec_buff));
    post_data = cJSON_PrintUnformatted_noescape(params);
    cJSON_Delete(params);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"post_newmsg_notice:rec(%s)",post_data);
    free(post_data);
    free(rec_buff);
    return NULL;
}

/*****************************************************************************
 函 数 名  : post_newmsg_notice
 功能描述  : 推送新消息提醒
 输入参数  : void arg  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int post_newmsg_notice(char* rid,char* targetid,char* msgpay,int server_flag)
{
	pthread_t task_id = 0;
    struct newmsg_notice_params* pmsg = NULL;

    if(rid == NULL || targetid == NULL || msgpay == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"post_newmsg_notice input err");
        return ERROR;
    }
    if((strlen(rid) > TOX_ID_STR_LEN) || (strlen(targetid) > TOX_ID_STR_LEN) || (strlen(msgpay) > CMD_MAXLEN))
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"post_newmsg_notice input params too long");
        return ERROR;
    }
    pmsg = (struct newmsg_notice_params*) malloc(sizeof(struct newmsg_notice_params));
    memset(pmsg,0,sizeof(struct newmsg_notice_params));
    pmsg->priority = PUSHMSG_PRI_LEVER_MIDDLE;
    pmsg->server_flag = server_flag;
    pmsg->type = PUSHMSG_TYPE_NOTICE_NEWMSG;
    strcpy(pmsg->from,rid);
    strcpy(pmsg->to,targetid);
    strcpy(pmsg->title,PNR_POSTMSG_TITLE);
    strcpy(pmsg->dev,g_dev_nickname);
    strcpy(pmsg->payload,msgpay);
    if (pthread_create(&task_id, NULL, pnr_post_newmsg_notice_task, pmsg) != 0) 
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pthread_create imuser_heartbeat_daemon failed");
        return ERROR;
    }
    return OK;
}
/**********************************************************************************
  Function:      pnr_post_newmsgs_notice_once
  Description:   新消息批量推送一次
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
static void *pnr_post_newmsgs_notice_once(void *para)
{
    struct newmsgs_notice_params* pmsg = (struct newmsgs_notice_params*)para;
    char* post_data = NULL;
    char rec_buff[BUF_LINE_MAX_LEN] = {0};
    int data_len = 0;
    cJSON * params = NULL;
    char server_url[URL_MAX_LEN] = {0};
    if(pmsg == NULL)
    {
        return NULL;
    }
    if(pmsg->server_flag == FALSE)
    {
        strcpy(server_url,PAPUSHMSG_DEVELOP_HTTPS_SERVER);
    }
    else
    {
        strcpy(server_url,PAPUSHMSG_PRODUCT_HTTPS_SERVER);
    }
    params = cJSON_CreateObject();
    if(params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"json create err");
        free(pmsg);
        return NULL;
    }
    cJSON_AddItemToObject(params, "from", cJSON_CreateString(pmsg->from));
    cJSON_AddItemToObject(params, "tos", cJSON_CreateString(pmsg->tos));
    cJSON_AddItemToObject(params, "title", cJSON_CreateString(pmsg->title));
    cJSON_AddItemToObject(params, "dev", cJSON_CreateString(pmsg->dev));
    cJSON_AddItemToObject(params, "payload", cJSON_CreateString(pmsg->payload));
    cJSON_AddItemToObject(params, "priority", cJSON_CreateNumber(pmsg->priority));
    cJSON_AddItemToObject(params, "type", cJSON_CreateNumber(pmsg->type));
    cJSON_AddItemToObject(params, "num", cJSON_CreateNumber(pmsg->to_num));
    cJSON_AddItemToObject(params, "version", cJSON_CreateNumber(pmsg->version));
    post_data = cJSON_PrintUnformatted_noescape(params);
    cJSON_Delete(params);
    data_len = strlen(post_data);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_post_newmsgs_notice_once:post_data(%s)",post_data);
    https_post(server_url,PAPUSHMSG_HTTPSSERVER_PORT,PAPUSHMSGS_HTTPSSERVER_PREURL,post_data,data_len,rec_buff,BUF_LINE_MAX_LEN);
    free(pmsg);
    free(post_data);
    params = cJSON_CreateObject();
    if(params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"json create err");
        return NULL;
    }
    cJSON_AddItemToObject(params, "replay", cJSON_CreateString(rec_buff));
    post_data = cJSON_PrintUnformatted_noescape(params);
    cJSON_Delete(params);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_post_newmsgs_notice_once:rec(%s)",post_data);
    free(post_data);
    return NULL;
}

/*****************************************************************************
 函 数 名  : post_newmsg_loop_task
 功能描述  : 轮询推送新消息推送
 输入参数  : void arg  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
void post_newmsgs_loop_task(void *para)
{
	pthread_t task_id = -1;
    struct newmsgs_notice_params*  pmsg = NULL;
    while(1)
    {
        if(g_post_usernum > 0 && g_post_usernum <= PNR_POST_USER_MAXNUM)
        {
            pmsg = (struct newmsgs_notice_params*)malloc(sizeof(struct newmsgs_notice_params));
            if(pmsg != NULL)
            {
                memset(pmsg,0,sizeof(struct newmsgs_notice_params));
                pthread_mutex_lock(&g_postlock);
                pmsg->to_num = g_post_usernum;
                strcpy(pmsg->tos,g_post_userstring);
                g_post_usernum = 0;
                memset(g_post_userstring,0,PNR_POST_USERSTR_MAXLEN);
                pthread_mutex_unlock(&g_postlock);
                pmsg->server_flag = TRUE;
                pmsg->version = PNR_POST_MSGS_VER1;
                strcpy(pmsg->from,g_daemon_tox.user_toxid);
                strcpy(pmsg->title,PNR_POSTMSG_TITLE);
                strcpy(pmsg->dev,g_dev_nickname);
                strcpy(pmsg->payload,PNR_POSTMSG_PAYLOAD);
                if (pthread_create(&task_id, NULL, pnr_post_newmsgs_notice_once, pmsg) != 0) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pthread_create pnr_post_newmsgs_notice_once failed");
                }
            }
            else
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"post msgs malloc failed");
            }
        }
        else if(g_post_usernum > PNR_POST_USER_MAXNUM)
        {
            pthread_mutex_lock(&g_postlock);
            g_post_usernum = 0 ;
            memset(g_post_userstring,0,PNR_POST_USERSTR_MAXLEN);
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"post over");
            pthread_mutex_unlock(&g_postlock);
        }
        sleep(PNR_REPEAT_TIME_1MIN);
    }    
    return;
}

/**********************************************************************************
  Function:      im_debug_pushnewnotice_deal
  Description:   测试推送新消息
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_debug_pushnewnotice_deal(char* pbuf)
{
    int user_index = 0;
    char* p_targetid = NULL;

    if(pbuf == NULL)
    {
        return ERROR;
    }
    if(strlen(pbuf) == TOX_ID_STR_LEN)
    {
        p_targetid = pbuf;
    }
    else
    {        
        user_index = atoi(pbuf);
        if(user_index <= 0 || user_index > PNR_IMUSER_MAXNUM)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_debug_pushnewnotice_deal bad userindex(%d)",user_index);
            return ERROR;
        }
        p_targetid = g_imusr_array.usrnode[user_index].user_toxid;
    }
    
    post_newmsg_notice(g_daemon_tox.user_toxid,p_targetid,PNR_POSTMSG_PAYLOAD,TRUE);
    return OK;
}
/**********************************************************************************
  Function:      im_debug_setfunc_deal
  Description:   动态设置功能开关
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_debug_setfunc_deal(char* pcmd)
{
    int cmd_type = 0;
    int param = FALSE;
    char* pbuf = NULL;
    if(pcmd == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_debug_setfunc_deal:bad input");
        return ERROR;
    }
    cmd_type = atoi(pcmd);
    switch(cmd_type)
    {
        case PNR_FUNCENABLE_NOTICE_NEWMSG:
            pbuf = strchr(pcmd,' ');
            if(pbuf == NULL)
            {
                return ERROR;
            }
            pbuf++;
            param = atoi(pbuf);
            if(param == FALSE)
            {
                g_noticepost_enable = FALSE;
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"set notice post switch = OFF");
            }
            else
            {
                g_noticepost_enable = TRUE;
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"set notice post switch = ON");
            }
            break;
        case PNR_FUNC_SET_LOGDEBUG_LEVER:
            pbuf = strchr(pcmd,' ');
            if(pbuf == NULL)
            {
                return ERROR;
            }
            pbuf++;
            param = atoi(pbuf);
            if(param >= DEBUG_LEVEL_INFO && param <= DEBUG_LEVEL_ERROR)
            {
                DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"set debug lever %d",param);
                if(g_debug_level != param)
                {
                    DEBUG_LEVEL(param);
                }
            }
            break;
        case PNR_FUNC_SET_UDP_RECDEBUGFLAG:
            pbuf = strchr(pcmd,' ');
            if(pbuf == NULL)
            {
                return ERROR;
            }
            pbuf++;
            param = atoi(pbuf);
            DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"set g_udpdebug_flag  %d",param);
            g_udpdebug_flag = param;
            break;
        case PNR_FUNC_SET_MSGDEBUG_FLAG:
            pbuf = strchr(pcmd,' ');
            if(pbuf == NULL)
            {
                return ERROR;
            }
            pbuf++;
            param = atoi(pbuf);
            if(param == FALSE)
            {
                g_msglist_debugswitch = FALSE;
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"set msglist debug switch = OFF");
            }
            else
            {
                g_msglist_debugswitch = TRUE;
                DEBUG_PRINT(DEBUG_LEVEL_INFO,"set msglist debug switch = ON");
            }
            break;
        default:
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_debug_setfunc_deal:bad cmd(%d)",cmd_type);
            break;
    }  
    return OK;
}
/**********************************************************************************
  Function:      pnr_relogin_pushbylws
  Description:   推送重复登陆的消息通过lws
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int pnr_relogin_pushbylws(int index,int type)
{
    int msglogid = 0;
    char* ret_buff = NULL;
    int n = 0;
	struct msg amsg;
    int retmsg_len = 0;
    struct per_session_data__minimal *pss = NULL;
	if (index <= 0 || index > g_imusr_array.max_user_num)
    {   
		return ERROR;
    }
    pss = g_imusr_array.usrnode[index].pss;
    if(pss == NULL)
    {
		return ERROR;
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    pnr_msgcache_getid(index,&msglogid);
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)msglogid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_LOGOUTPUSH));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(g_imusr_array.usrnode[index].user_toxid));
    cJSON_AddItemToObject(ret_params, "RouterId", cJSON_CreateString(g_daemon_tox.user_toxid));
    cJSON_AddItemToObject(ret_params, "Reason", cJSON_CreateNumber(type));
    if(type == PNR_PUSHLOGOUT_REASON_RELOGIN)
    {
        cJSON_AddItemToObject(ret_params, "Info", cJSON_CreateString(PNR_PUSHLOGOUT_RELOGIN_STRING));
    }
    else if(type == PNR_PUSHLOGOUT_REASON_DELUSER)
    {
        cJSON_AddItemToObject(ret_params, "Info", cJSON_CreateString(PNR_PUSHLOGOUT_DELACCOUNT_STRING));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    retmsg_len = strlen(ret_buff);
    pthread_mutex_lock(&pss->lock_ring);
            
    /* only create if space in ringbuffer */
    n = (int)lws_ring_get_count_free_elements(pss->ring);
    if (!n) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "dropping!");
        free(ret_buff);
        pthread_mutex_unlock(&pss->lock_ring);
        return ERROR;
    }
    
    amsg.payload = malloc(LWS_PRE + retmsg_len + 1);
    if (!amsg.payload) {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "OOM: dropping malloc(%d)",retmsg_len);
        free(ret_buff);
        pthread_mutex_unlock(&pss->lock_ring);
        return ERROR;
    }
    
    memset(amsg.payload, 0, LWS_PRE + retmsg_len + 1);
    strncpy((char *)amsg.payload + LWS_PRE, ret_buff, retmsg_len);
    free(ret_buff);
    amsg.len = retmsg_len;
    DEBUG_PRINT(DEBUG_LEVEL_INFO, "copy(%d:%s)!",retmsg_len,(amsg.payload + LWS_PRE));
    n = lws_ring_insert(pss->ring, &amsg, 1);
    if (n != 1) 
    {
        __minimal_destroy_message(&amsg);
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "dropping!");
        pthread_mutex_unlock(&pss->lock_ring);
        return ERROR;
    }

    //清除原index
    //pss->user_index = 0;
    pthread_mutex_unlock(&pss->lock_ring);

    if (g_lws_handler[index])
    {
        lws_callback_on_writable(g_lws_handler[index]);
    } 
    else 
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR, "wsi null!");
        return ERROR;
    }
    return OK;
}
/**********************************************************************************
  Function:      pnr_relogin_pushbytox
  Description:   推送重复登陆的消息通过tox
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int pnr_relogin_pushbytox(int index,int type)
{
    int msglogid = 0;
    char* ret_buff = NULL;
    int retmsg_len = 0;
    int friend_num = 0;
	if (index <= 0 || index > g_imusr_array.max_user_num)
    {   
		return ERROR;
    }
    friend_num = g_imusr_array.usrnode[index].appid;
    if(friend_num < 0)
    {
		return ERROR;
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    pnr_msgcache_getid(index,&msglogid);
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V2));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)msglogid));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_LOGOUTPUSH));
    cJSON_AddItemToObject(ret_params, "UserId", cJSON_CreateString(g_imusr_array.usrnode[index].user_toxid));
    cJSON_AddItemToObject(ret_params, "RouterId", cJSON_CreateString(g_daemon_tox.user_toxid));
    cJSON_AddItemToObject(ret_params, "Reason", cJSON_CreateNumber(type));
    if(type == PNR_PUSHLOGOUT_REASON_RELOGIN)
    {
        cJSON_AddItemToObject(ret_params, "Info", cJSON_CreateString(PNR_PUSHLOGOUT_RELOGIN_STRING));
    }
    else if(type == PNR_PUSHLOGOUT_REASON_DELUSER)
    {
        cJSON_AddItemToObject(ret_params, "Info", cJSON_CreateString(PNR_PUSHLOGOUT_DELACCOUNT_STRING));
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    ret_buff = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    retmsg_len = strlen(ret_buff);
            
  	tox_friend_send_message(g_daemon_tox.ptox_handle,friend_num,TOX_MESSAGE_TYPE_NORMAL, 
    	(uint8_t *)ret_buff, retmsg_len, NULL);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"resp(%d:%s)",retmsg_len,ret_buff);
    free(ret_buff);
    return OK;
}

/**********************************************************************************
  Function:      pnr_relogin_push
  Description:   检测是否重复登陆，如果是重复登陆，推送消息给前一个用户
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int pnr_relogin_push(int index,int curtox_flag,int cur_fnum,struct per_session_data__minimal *cur_pss)
{
	if (index <= 0 || index > g_imusr_array.max_user_num)
    {   
		return ERROR;
    }
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"curtox_flag(%d) user(%d) user_online_type(%d)",
        curtox_flag,index,g_imusr_array.usrnode[index].user_online_type);
    if(curtox_flag)
    {
        if(cur_fnum < 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_relogin_push: input cur_fnun err");
            return ERROR;
        }
        if(g_imusr_array.usrnode[index].user_online_type == USER_ONLINE_TYPE_LWS)
        {
            pnr_relogin_pushbylws(index,PNR_PUSHLOGOUT_REASON_RELOGIN);
        }
        else if(cur_fnum != g_imusr_array.usrnode[index].appid)
        {
            pnr_relogin_pushbytox(index,PNR_PUSHLOGOUT_REASON_RELOGIN);
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_relogin_push:user(%d) relogin same1");
        }
    }
    else
    {
        if(cur_pss == NULL)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_relogin_push: input cur_pss err");
            return ERROR;
        }
        if(g_imusr_array.usrnode[index].user_online_type == USER_ONLINE_TYPE_TOX)
        {
            pnr_relogin_pushbytox(index,PNR_PUSHLOGOUT_REASON_RELOGIN);
        }
        else if(cur_pss != g_imusr_array.usrnode[index].pss)
        {
            pnr_relogin_pushbylws(index,PNR_PUSHLOGOUT_REASON_RELOGIN);
        }
        else
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_relogin_push:user(%d) relogin same1");
        }
    }
    return OK;
}
char g_simulation_msgretbuf[IM_JSON_MAXLEN+1] = {0};
int g_simulation_num = 0;
pthread_t simulation_task[PNR_IMUSER_FRIENDS_MAXNUM+1];
#define SIMULATION_INDEX_OFFSET 5 
#define SIMULATION_SIGN  "Ln2DeZZ7ni/LJJr/j/mjQGF7BO6RxZOWcBYxe8YMPrJZFAAogRtATWsG7oAyqrJX+TMj19oTMV+1Mv4WtjoABjE1NTIzNzE3NDkAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
#define SIMULATION_MSG  "+41Kotz4k35C3ebm/l3sbwBMhzo0ml7H"
#define SIMULATION_NONCE  "S1NS3e8wfD7d8zT1POy0Eb4ARWI7G/JD"
#define SIMULATION_PRIKEY  "dEU0p+MREU/eQLK7xnQtBRAoyq+CtTii3EhHoEOnRDVVbh0IUNwFj9OuihF1BXPT0nl17+ohtmcdR3EJtGJKT/AMx36VGYP4b+FYLpA7sl4="
#define SIMULATION_MSGSIGN  "s6+Exn7t7i4MgvQ9oMlNaEtuV71Y9WX4JBdx748whlb0WSsCB7vQK44BsCRKHcRKERABDKBhcG5HI3pIwvvTDPnDnGRLXD5e/+Eq0ph9mWIF90yC9zdqvsPJvvAw2D54"
/**********************************************************************************
  Function:      pnr_simulation_user_task
  Description:   模拟用户任务
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
static void *pnr_simulation_user_task(void *para)
{
	cJSON *root =  NULL;
    cJSON *params = NULL;
    char username[PNR_USERNAME_MAXLEN+1] = {0};
    char username_encode[PNR_USERNAME_MAXLEN+1] = {0};
    int username_len = 0;
    char from_id[TOX_ID_STR_LEN+1] = {0};
    int msg_id = 0;
    struct imcmd_msghead_struct msg_head;
    struct pnr_account_struct src_account;
    int pws_index = 0;
    int uindex = *(int*)para;
    int ret_len = 0;
    msg_id = uindex*10000;
    if(uindex <=0 || uindex > PNR_IMUSER_MAXNUM)
    {
        return NULL;
    }
    snprintf(username,PNR_USERNAME_MAXLEN,"simulat_user%d",uindex);
    if(pnr_base64_encode(username,strlen(username),username_encode,&username_len) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_simulation_user_task:pnr_base64_encode error");
        return NULL;
    }
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_simulation_user_task:user(%d:%s)",uindex,g_imusr_array.usrnode[uindex].user_toxid);
    //没有实例化的用户，需要先注册或者登陆激活
    if(g_imusr_array.usrnode[uindex].init_flag != TRUE)
    {
        //登陆
        if(g_imusr_array.usrnode[uindex].user_toxid[0] != 0)
        {
            memset(&src_account,0,sizeof(src_account));
            strcpy(src_account.toxid,g_imusr_array.usrnode[uindex].user_toxid);
            pnr_account_dbget_byuserid(&src_account);
            root =  cJSON_CreateObject();
            params = cJSON_CreateObject();
            if(root == NULL || params == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                cJSON_Delete(root);
                return NULL;
            }
            cJSON_AddItemToObject(root, "appid", cJSON_CreateString("MIFI"));
            cJSON_AddItemToObject(root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
            cJSON_AddItemToObject(root, "apiversion", cJSON_CreateNumber(PNR_API_VERSION_V4));
        	cJSON_AddItemToObject(root, "msgid", cJSON_CreateNumber((double)msg_id));

            cJSON_AddItemToObject(params, "Action", cJSON_CreateString(PNR_IMCMD_LOGIN));
            cJSON_AddItemToObject(params, "RouteId", cJSON_CreateString(g_daemon_tox.user_toxid));
            cJSON_AddItemToObject(params, "UserSn", cJSON_CreateString(src_account.user_sn));
            cJSON_AddItemToObject(params, "UserId", cJSON_CreateString(g_imusr_array.usrnode[uindex].user_toxid));
            cJSON_AddItemToObject(params, "Sign", cJSON_CreateString(SIMULATION_SIGN));
            cJSON_AddItemToObject(params, "NickName", cJSON_CreateString(src_account.nickname));
            cJSON_AddItemToObject(params, "DataFileVersion", cJSON_CreateNumber(1));
            cJSON_AddItemToObject(root, "params", params);
        }
        else//注册
        {
            root =  cJSON_CreateObject();
            params = cJSON_CreateObject();
            if(root == NULL || params == NULL)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
                cJSON_Delete(root);
                return NULL;
            }
            cJSON_AddItemToObject(root, "appid", cJSON_CreateString("MIFI"));
            cJSON_AddItemToObject(root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
            cJSON_AddItemToObject(root, "apiversion", cJSON_CreateNumber(PNR_API_VERSION_V4));
        	cJSON_AddItemToObject(root, "msgid", cJSON_CreateNumber((double)msg_id));

            cJSON_AddItemToObject(params, "Action", cJSON_CreateString(PNR_IMCMD_REGISTER));
            cJSON_AddItemToObject(params, "RouteId", cJSON_CreateString(g_daemon_tox.user_toxid));
            cJSON_AddItemToObject(params, "UserSn", cJSON_CreateString(g_account_array.temp_user_sn));
            cJSON_AddItemToObject(params, "NickName", cJSON_CreateString(username_encode));
            cJSON_AddItemToObject(params, "Sign", cJSON_CreateString(SIMULATION_SIGN));
            cJSON_AddItemToObject(params, "Pubkey", cJSON_CreateString(username_encode));
            cJSON_AddItemToObject(root, "params", params);
        }
        //消息解析
        memset(&msg_head,0,sizeof(msg_head));
        if(im_msghead_parses(root,params,&msg_head) != OK)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_msghead_parses failed");
            return NULL;
        }
        switch(msg_head.im_cmdtype)
        {
            case PNR_IM_CMDTYPE_REGISTER:
                if (im_user_register_v4_deal(params, g_simulation_msgretbuf, &ret_len, &pws_index, &msg_head))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_user_register_v4_deal failed");
                    return NULL;
                }
                memset(&src_account,0,sizeof(src_account));
                strcpy(src_account.user_pubkey,username_encode);
                pnr_account_dbget_byuserkey(&src_account);
                strcpy(from_id,src_account.toxid);
                break;
            case PNR_IM_CMDTYPE_LOGIN:
                if (im_userlogin_v4_deal(params,g_simulation_msgretbuf,&ret_len, &pws_index,&msg_head,-1,NULL,TRUE))
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "im_userlogin_v4_deal failed");
                    return NULL;
                }
                strcpy(from_id,g_imusr_array.usrnode[uindex].user_toxid);
                break;
            default:
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad cmd(%d)",msg_head.im_cmdtype);
                break;
        }
        cJSON_Delete(root);
    }
    if(strlen(from_id) > 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"user(%d:%s) simulation run",uindex,from_id);
    }
    else
    {
        DEBUG_PRINT(DEBUG_LEVEL_NORMAL,"user(%d) simulation exit",uindex);
        return NULL;
    }
    //然后定期发消息
    root =  cJSON_CreateObject();
    params = cJSON_CreateObject();
    if(root == NULL || params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(root);
        return NULL;
    }
    msg_id++;
    cJSON_AddItemToObject(root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(root, "apiversion", cJSON_CreateNumber(PNR_API_VERSION_V4));
	cJSON_AddItemToObject(root, "msgid", cJSON_CreateNumber((double)msg_id));
    
    cJSON_AddItemToObject(params, "Action", cJSON_CreateString(PNR_IMCMD_SENDMSG));
    cJSON_AddItemToObject(params, "From", cJSON_CreateString(from_id));
    cJSON_AddItemToObject(params, "To", cJSON_CreateString(g_account_array.account[PNR_ADMINUSER_PSN_INDEX].toxid));
    cJSON_AddItemToObject(params, "Msg", cJSON_CreateString(SIMULATION_MSG));
    cJSON_AddItemToObject(params, "Sign", cJSON_CreateString(SIMULATION_MSGSIGN));
    cJSON_AddItemToObject(params, "Nonce", cJSON_CreateString(SIMULATION_NONCE));
    cJSON_AddItemToObject(params, "PriKey", cJSON_CreateString(SIMULATION_PRIKEY));
    cJSON_AddItemToObject(root, "params", params);
    memset(&msg_head,0,sizeof(msg_head));
    if(im_msghead_parses(root,params,&msg_head) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_msghead_parses failed");
        return NULL;
    }
    while(1)
    {
        msg_head.msgid++;
        if(im_sendmsg_cmd_deal_v3(params, g_simulation_msgretbuf, &ret_len, &pws_index, &msg_head) != OK)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_sendmsg_cmd_deal failed");
            return NULL;
        }
        sleep(2);
    }
    return NULL;
}
/**********************************************************************************
  Function:      im_simulation_setfunc_deal
  Description:  模拟测试功能开关
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
int im_simulation_setfunc_deal(char* pcmd)
{
    int simulation_num = 0;
    int i =0,uindex = 0;
    if(pcmd == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"im_debug_setfunc_deal:bad input");
        return ERROR;
    }
    simulation_num = atoi(pcmd);
    if(simulation_num < 0 || simulation_num > PNR_IMUSER_MAXNUM)
    {
        return ERROR;
    }
    else if(simulation_num == 0)
    {
        if(g_simulation_num != 0)
        {
            for(i=1;i<=g_simulation_num;i++)
            {
                pthread_cancel(simulation_task[i]);
            }
            g_simulation_num = 0;
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_simulation_setfunc_deal: disable");
    }
    else
    {
        if(g_simulation_num != 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_simulation_setfunc_deal: already run");
        }
        else
        {
            for(i=1;i<=simulation_num;i++)
            { 
                uindex = i + SIMULATION_INDEX_OFFSET;
                if(uindex >= PNR_IMUSER_MAXNUM)
                {
                    break;
                }
                if (pthread_create(&simulation_task[i], NULL, pnr_simulation_user_task, &uindex) != 0) 
                {
                    DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pthread_create pnr_simulation_user_task failed");
                    return ERROR;
                }
                sleep(1);
            }
            g_simulation_num = i;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"im_simulation_setfunc_deal: enable run %d simulation",g_simulation_num);
        }
       
    }
    return OK;
}
/*****************************************************************************
 函 数 名  : pnr_frpc_setconfig
 功能描述  : pnr frpc代理服务配置
 输入参数  : 
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_frpc_setconfig(struct pnrdev_netconn_info* pnetinfo)
{
    char cmd[CMD_MAXLEN] = {0};
    FILE* fp = NULL;

    if(pnetinfo == NULL)
    {
        return ERROR;
    }
    if(pnetinfo->frp_mode == FALSE)
    {
        return ERROR;
    }
    if(g_pnrdevtype == PNR_DEV_TYPE_ONESPACE)
    {
        system("mount -o remount,rw /");
    }
    unlink(PNR_FRPC_CONFIG_TMPFILE);
    fp = fopen(PNR_FRPC_CONFIG_TMPFILE, "w+");
    if(fp < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"open faile");
        return ERROR;
    }
    snprintf(cmd,CMD_MAXLEN,"[common]\nserver_addr = %s\nserver_port = %d\n\n",pnetinfo->pub_ipstr,pnetinfo->frp_port);
    fputs(cmd, fp);
    if(pnetinfo->frp_mode & PNRDEV_FRPCONNCT_SSHSEVER)
    {
        memset(cmd,0,CMD_MAXLEN);
        snprintf(cmd,CMD_MAXLEN,"[ssh%d]\ntype = tcp\nlocal_ip = 127.0.0.1\nlocal_port = 22\nremote_port = %d\n\n",pnetinfo->pnr_port,pnetinfo->ssh_port);
        fputs(cmd, fp);
    }
    if(pnetinfo->frp_mode & PNRDEV_FRPCONNCT_PNRSEVER)
    {
        memset(cmd,0,CMD_MAXLEN);
        snprintf(cmd,CMD_MAXLEN,"[r%d_ws1]\ntype = tcp\nlocal_ip = 127.0.0.1\nlocal_port = 18006\nremote_port = %d\n\n",pnetinfo->pnr_port,pnetinfo->pnr_port);
        fputs(cmd, fp);
        memset(cmd,0,CMD_MAXLEN);
        snprintf(cmd,CMD_MAXLEN,"[r%d_ws2]\ntype = tcp\nlocal_ip = 127.0.0.1\nlocal_port = 18007\nremote_port = %d\n\n",pnetinfo->pnr_port,(pnetinfo->pnr_port+1));
        fputs(cmd, fp);
    }
    fclose(fp);
    memset(cmd,0,CMD_MAXLEN);
    snprintf(cmd,CMD_MAXLEN,"mv -f %s %s",PNR_FRPC_CONFIG_TMPFILE,PNR_FRPC_CONFIG_FILE);
    system(cmd);
    return OK;
}

/*****************************************************************************
 函 数 名  : pnr_frpc_enable
 功能描述  : pnr frpc代理服务配置
 输入参数  : 
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_frpc_enable(int enable)
{
    char cmd[CMD_MAXLEN] = {0};
    
    if(enable == TRUE)
    {
        if(g_pnrdevtype == PNR_DEV_TYPE_ONESPACE)
        {
            system("mount -o remount,rw /");
        }
        system("killall frpc");
        snprintf(cmd,CMD_MAXLEN,"frpc -c %s >> /tmp/frp.log &",PNR_FRPC_CONFIG_FILE);
    }
    else
    {
        unlink(PNR_FRPC_CONFIG_FILE);
        snprintf(cmd,CMD_MAXLEN,"killall frpc");
    }
    system(cmd);
    return OK;
}

/*****************************************************************************
 函 数 名  : pnr_devinfo_get
 功能描述  : 设备信息获取
 输入参数  : 
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_devinfo_get(struct pnrdev_register_info* pinfo)
{
    char fullmac[MACSTR_MAX_LEN+1] = {0};
    if(pinfo == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_devinfo_get:input err");
        return ERROR;
    }
    if(strlen(g_daemon_tox.user_toxid) < TOX_ID_STR_LEN)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_devinfo_get:g_daemon_tox.user_toxid not ok");
        return ERROR;
    }
    memset(pinfo,0,sizeof(struct pnrdev_register_info));
    pinfo->dev_type = g_pnrdevtype;
    strcpy(pinfo->rid,g_daemon_tox.user_toxid);
    get_hwaddr_byname(DEV_ETH0_KEYNAME,fullmac,pinfo->eth0_mac);
    get_hwaddr_byname(DEV_ETH1_KEYNAME,fullmac,pinfo->eth1_mac);
    get_localip_byname(DEV_ETH0_KEYNAME,pinfo->eth0_localip);
    get_localip_byname(DEV_ETH1_KEYNAME,pinfo->eth1_localip);
    snprintf(pinfo->version,PNR_QRCODE_MAXLEN,"%s.%s.%s:%s",
        PNR_SERVER_TOPVERSION,PNR_SERVER_MIDVERSION,
        PNR_SERVER_LOWVERSION,PNR_SERVER_BUILD_TIME);
    return OK;
}

/**********************************************************************************
  Function:      pnr_dev_register_task
  Description:   设备启动之后注册
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
void *pnr_dev_register_task(void *para)
{
    char* tmp_json_buff = NULL;
    cJSON* tmp_item = NULL;
    struct pnrdev_register_info devinfo;
    struct pnrdev_netconn_info netinfo;
    struct pnrdev_register_resp resp;
    char* post_data = NULL;
    char rec_buff[BUF_LINE_MAX_LEN] = {0};
    int data_len = 0;
    cJSON * params = NULL;
    int pid = 0;
    int changeflag = FALSE;
    while(g_p2pnet_init_flag != TRUE)
    {
        sleep(1);
    }
    g_devreg_repeat_time = PNR_REPEAT_TIME_1MIN;
    //获取当前设备信息
    if(pnr_devinfo_get(&devinfo) != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_devinfo_get err");
        return NULL;
    }
    pnr_netconfig_dbget(&netinfo);

    //如果是启动了frp代理，检测frp连接状态
    if(netinfo.pubnet_mode == PNRDEV_NETCONN_FRPPROXY)
    {
        pnr_check_process_byname("frpc",&pid);
        if(pid <= 0)
        {
            netinfo.conn_status = FALSE;
        }
        else if(pnr_check_frp_connstatus() == TRUE)
        {
            netinfo.conn_status = TRUE;
        }
        else
        {
            netinfo.conn_status = FALSE;
        }
    }
    params = cJSON_CreateObject();
    if(params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"json create err");
        return NULL;
    }
    cJSON_AddItemToObject(params, "Dev", cJSON_CreateNumber(devinfo.dev_type));
    cJSON_AddItemToObject(params, "Rid", cJSON_CreateString(devinfo.rid));
    cJSON_AddItemToObject(params, "Version", cJSON_CreateString(devinfo.version));
    cJSON_AddItemToObject(params, "E0Mac", cJSON_CreateString(devinfo.eth0_mac));
    cJSON_AddItemToObject(params, "E1Mac", cJSON_CreateString(devinfo.eth1_mac));
    cJSON_AddItemToObject(params, "E0Ip", cJSON_CreateString(devinfo.eth0_localip));
    cJSON_AddItemToObject(params, "E1Ip", cJSON_CreateString(devinfo.eth1_localip));
    cJSON_AddItemToObject(params, "PubNetMode", cJSON_CreateNumber(netinfo.pubnet_mode));
    cJSON_AddItemToObject(params, "FrpMode", cJSON_CreateNumber(netinfo.frp_mode));
    cJSON_AddItemToObject(params, "ConnStat", cJSON_CreateNumber(netinfo.conn_status));
    cJSON_AddItemToObject(params, "PnrPort", cJSON_CreateNumber(netinfo.pnr_port));
    cJSON_AddItemToObject(params, "SshPort", cJSON_CreateNumber(netinfo.ssh_port));
    cJSON_AddItemToObject(params, "FrpPort", cJSON_CreateNumber(netinfo.frp_port));
    cJSON_AddItemToObject(params, "PubIp", cJSON_CreateString(netinfo.pub_ipstr));
    cJSON_AddItemToObject(params, "NodeName", cJSON_CreateString(g_dev_nickname));
    cJSON_AddItemToObject(params, "Info", cJSON_CreateString(netinfo.attach));
    post_data = cJSON_PrintUnformatted_noescape(params);
    cJSON_Delete(params);
    data_len = strlen(post_data);
    //DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_dev_register_task:post_data(%s)",post_data);
    //云端注册
    //https_post(PAPUSHMSG_DEVELOP_HTTPS_SERVER,PAPUSHMSG_HTTPSSERVER_PORT,PNR_HTTPSSERVER_DEVREGISTER,post_data,data_len,rec_buff,BUF_LINE_MAX_LEN);
    if(https_post(PAPUSHMSG_PRODUCT_HTTPS_SERVER,PAPUSHMSG_HTTPSSERVER_PORT,PNR_HTTPSSERVER_DEVREGISTER,post_data,data_len,rec_buff,BUF_LINE_MAX_LEN) < 0)
    {
        free(post_data);
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_dev_register_task:https_post fail");
        return NULL;
    }
    free(post_data);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_dev_register_task:rec(%s)",rec_buff);
    //返回值处理
	params = cJSON_Parse(rec_buff);
    if(params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_dev_register_task:ret(%s) Parse fail",rec_buff);
        return NULL;
    }
    memset(&resp,0,sizeof(struct pnrdev_register_resp));
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Ret",resp.ret,0);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"Renew",resp.renew_flag,0);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"PubNetMode",resp.pubnet_mode,0);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"FrpMode",resp.frp_mode,0);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"FrpPort",resp.frp_port,0);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"PnrPort",resp.pnr_port,0);
    CJSON_GET_VARINT_BYKEYWORD(params,tmp_item,tmp_json_buff,"SshPort",resp.ssh_port,0);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"PubIp",resp.pub_ipstr,IPSTR_MAX_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"Rid",resp.rid,TOX_ID_STR_LEN);
    CJSON_GET_VARSTR_BYKEYWORD(params,tmp_item,tmp_json_buff,"E0Mac",resp.eth0_mac,MACSTR_MAX_LEN);
    if(resp.ret != OK)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad ret(%d)",resp.ret);
        return NULL;
    }
    g_devreg_repeat_time = PNR_REPEAT_TIME_15MIN;
    if(resp.frp_mode != netinfo.frp_mode)
    {
        netinfo.frp_mode = resp.frp_mode;
        changeflag = TRUE;
        dbupdate_intvalue_byname(g_db_handle,"generconf_tbl",DB_FRPMODE_KEYWORD,netinfo.frp_mode);
    }
    if(resp.pubnet_mode != netinfo.pubnet_mode)
    {
        netinfo.pubnet_mode = resp.pubnet_mode;
        changeflag = TRUE;
        dbupdate_intvalue_byname(g_db_handle,"generconf_tbl",DB_PUBNETMODE_KEYWORD,netinfo.pubnet_mode);
    }
    if(resp.pnr_port != netinfo.pnr_port)
    {
        netinfo.pnr_port = resp.pnr_port;
        changeflag = TRUE;
        dbupdate_intvalue_byname(g_db_handle,"generconf_tbl",DB_PUBNET_PORT_KEYWORD,netinfo.pnr_port);
    }
    if(resp.ssh_port != netinfo.ssh_port)
    {
        netinfo.ssh_port = resp.ssh_port;
        changeflag = TRUE;
        dbupdate_intvalue_byname(g_db_handle,"generconf_tbl",DB_PUBNET_SSHPORT_KEYWORD,netinfo.ssh_port);
    }
    if(resp.frp_port != netinfo.frp_port)
    {
        netinfo.frp_port = resp.frp_port;
        changeflag = TRUE;
        dbupdate_intvalue_byname(g_db_handle,"generconf_tbl",DB_FRPPORT_KEYWORD,netinfo.frp_port);
    }
    if(strcmp(netinfo.pub_ipstr,resp.pub_ipstr) != OK)
    {
        memset(netinfo.pub_ipstr,0,IPSTR_MAX_LEN);
        strcpy(netinfo.pub_ipstr,resp.pub_ipstr);
        changeflag = TRUE;
        dbupdate_strvalue_byname(g_db_handle,"generconf_tbl",DB_PUBNET_IPSTR_KEYWORD,netinfo.pub_ipstr);
    }
    //如果网络配置有变化，修改
    if(resp.renew_flag == TRUE || changeflag == TRUE)
    {
        if(netinfo.pubnet_mode != PNRDEV_NETCONN_FRPPROXY)
        {
           //停止ftpc服务
           pnr_frpc_enable(FALSE);
        }
        else if(resp.frp_mode != 0)
        {
            //生成新的frp配置
            pnr_frpc_setconfig(&netinfo);
            pnr_frpc_enable(TRUE);
        }
    }
    //检测如果是frp模式
    if(netinfo.pubnet_mode == PNRDEV_NETCONN_FRPPROXY)
    {
        if(access(PNR_FRPC_CONFIG_FILE,F_OK) != OK)
        {
            pnr_frpc_setconfig(&netinfo);
            pnr_frpc_enable(TRUE);
        }
        pnr_check_process_byname("frpc",&pid);
        if(pid <= 0)
        {
            pnr_frpc_enable(TRUE);
        }
    }
    cJSON_Delete(params);
    return NULL;
}
/*****************************************************************************
 函 数 名  : post_devinfo_upload_task
 功能描述  : 推送设备注册信息
 输入参数  : void arg  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
void post_devinfo_upload_task(void *para)
{
	pthread_t task_id = 0;
    void *status;
    int first_flag = TRUE;
    while(1)
    {
        //第一次需要滞后1min，等网络起来
        if(first_flag == TRUE)
        {
            sleep(PNR_REPEAT_TIME_30SEC);
            first_flag = FALSE;
        }
        if (pthread_create(&task_id, NULL, pnr_dev_register_task, NULL) != 0) 
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pthread_create post_devinfo_register failed");
        }
        pthread_join(task_id,&status);
        //DEBUG_PRINT(DEBUG_LEVEL_ERROR,"post_devinfo_register_task:sleep(%d)",g_devreg_repeat_time);
        sleep(g_devreg_repeat_time);
    }    
    return;
}
int post_devinfo_upload_once(void* param)
{
    pnr_dev_register_task(NULL);
    return OK;
}
/*****************************************************************************
 函 数 名  : pnr_rnode_msg_push
 功能描述  : rid节点消息推送
 输入参数  : void arg  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_rnode_msg_push(int cmd,char* to_id,void* param1,void* param2)
{
    int f_id=0,i = 0,ret = 0;
    char* pmsg = NULL;
    if(cmd < PNR_SYSDEBUG_CMDTYPE_CHECK_SYSINFO || cmd >= PNR_SYSDEBUG_CMDTYPE_BUTT)
    {
        return ERROR;
    }
    if(to_id == NULL || strlen(to_id) != TOX_ID_STR_LEN)
    {
        return ERROR;
    }
    if(((cmd == PNR_SYSDEBUG_CMDTYPE_POST_DEBUGINFO || cmd == PNR_SYSDEBUG_CMDTYPE_ACTIVE_USER) &&(param1 == NULL))
        ||((cmd == PNR_SYSDEBUG_CMDTYPE_CHECK_FRIENDS || cmd == PNR_SYSDEBUG_CMDTYPE_CHECKANDADD_FRIENDS) && (param1 == NULL || param2 == NULL )))
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"input cmd(%d) params null",cmd);
        return ERROR;
    }
    f_id = get_rnodefidbytoxid(to_id);
    if(f_id < 0)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"input cmd(%d) params null",cmd);
        return ERROR;
    }
    else if(f_id == 0xFF)
    {
        for( i = 1; i < PNR_IMUSER_MAXNUM ; i++ )
        {
            if(g_rnode[i].tox_id[0] == 0)
            {
                f_id = check_and_add_friends(g_daemon_tox.ptox_handle,to_id,g_daemon_tox.userinfo_fullurl);
                if(f_id < 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO, "check add friend(%s) failed ret(%d)",to_id,f_id);
                    return ERROR;
                }
                else
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO, "check add friend(%s) OK",to_id);
                    g_rnode[i].f_id = f_id;
                    g_rnode[i].c_status = PNR_RID_NODE_CSTATUS_CONNETTING;
                }
                strncpy(g_rnode[i].tox_id,to_id,TOX_ID_STR_LEN);
            }
        }
    }
    //构建响应消息
    cJSON * ret_root =  cJSON_CreateObject();
    cJSON * ret_params =  cJSON_CreateObject();
    if(ret_root == NULL || ret_params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"err");
        cJSON_Delete(ret_root);
        return ERROR;
    }
    cJSON_AddItemToObject(ret_root, "appid", cJSON_CreateString("MIFI"));
    cJSON_AddItemToObject(ret_root, "timestamp", cJSON_CreateNumber((double)time(NULL)));
    cJSON_AddItemToObject(ret_root, "apiversion", cJSON_CreateNumber((double)PNR_API_VERSION_V4));
    cJSON_AddItemToObject(ret_root, "msgid", cJSON_CreateNumber((double)time(NULL)));

    cJSON_AddItemToObject(ret_params, "Action", cJSON_CreateString(PNR_IMCMD_SYSDEBUGCMD));
    cJSON_AddItemToObject(ret_params, "FromRid",cJSON_CreateString(g_daemon_tox.user_toxid));
    cJSON_AddItemToObject(ret_params, "Cmd",cJSON_CreateNumber(cmd));
    cJSON_AddItemToObject(ret_params, "ToRid",cJSON_CreateString(to_id));
    switch(cmd)
    {
        case PNR_SYSDEBUG_CMDTYPE_CHECK_SYSINFO:
        case PNR_SYSDEBUG_CMDTYPE_CHECK_NETINFO:
        case PNR_SYSDEBUG_CMDTYPE_REFLUSH_DEVREG:
            break;
        case PNR_SYSDEBUG_CMDTYPE_POST_DEBUGINFO:
            cJSON_AddItemToObject(ret_params, "User",cJSON_CreateString(param1));
            break;
        case PNR_SYSDEBUG_CMDTYPE_ACTIVE_USER:
            cJSON_AddItemToObject(ret_params, "User",cJSON_CreateString(param1));
            break;
        case PNR_SYSDEBUG_CMDTYPE_CHECK_FRIENDS:
        case PNR_SYSDEBUG_CMDTYPE_CHECKANDADD_FRIENDS:
            cJSON_AddItemToObject(ret_params, "User",cJSON_CreateString(param1));
            cJSON_AddItemToObject(ret_params, "Friend",cJSON_CreateString(param2));
            break;
         default:
            break;   
    }
    cJSON_AddItemToObject(ret_root, "params", ret_params);
    pmsg = cJSON_PrintUnformatted(ret_root);
    cJSON_Delete(ret_root);
    ret = tox_friend_get_connection_status(g_daemon_tox.ptox_handle, f_id, NULL);
    if (ret == TOX_CONNECTION_TCP || ret == TOX_CONNECTION_UDP) 
    {
        tox_friend_send_message(g_daemon_tox.ptox_handle,f_id, TOX_MESSAGE_TYPE_NORMAL, 
            (uint8_t *)pmsg, strlen(pmsg), NULL);
    }
    else
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"f_id(%d:%s) connect not ready",f_id,to_id);
    }
    free(pmsg);
    return OK;
}
/*****************************************************************************
 函 数 名  : pnr_rnode_debugmsg_send
 功能描述  : rid节点调试消息发送
 输入参数  : void arg  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_rnode_debugmsg_send(char* pcmd)
{
    int cmdtype = 0;
    char* pbuf = NULL;
    char* p_target_rid = NULL;
    char* param1 = NULL;
    char* param2 = NULL;
    if(pcmd == NULL)
    {
        return ERROR;
    }
    cmdtype = atoi(pcmd);
    pbuf = strchr(pcmd,' ');
    if(pbuf == NULL)
    {
        return ERROR;
    }
    pbuf++;
    p_target_rid = pbuf;
    pbuf = strchr(p_target_rid,' ');
    if(pbuf != NULL)
    {
        pbuf[0] = 0;
        pbuf++;
        param1 = pbuf;
        pbuf = strchr(param1,' ');
        if(pbuf != NULL)
        {
            pbuf[0] = 0;
            pbuf++;
            param2 = pbuf;
        }
    }
    return pnr_rnode_msg_push(cmdtype,p_target_rid,param1,param2);
}
/*****************************************************************************
 函 数 名  : pnr_router_node_friend_init
 功能描述  : 节点启动后，所有有好友关系rid互相加好友
 输入参数  : void arg  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_router_node_friend_init(void)
{
    char **dbResult; 
    char *errmsg;
    int nRow, nColumn,offset,i;
    int f_num = 0;
    char sql_cmd[SQL_CMD_LEN+1] = {0};

    memset(g_rnode,0,sizeof(struct pnr_rid_node) * (PNR_IMUSER_MAXNUM+1));
    snprintf(sql_cmd,SQL_CMD_LEN,"select devid,devname from userdev_mapping_tbl where devid !='%s' group by devid;",g_daemon_tox.user_toxid);
    DEBUG_PRINT(DEBUG_LEVEL_INFO, "sql_cmd(%s)",sql_cmd);
    if(sqlite3_get_table(g_db_handle, sql_cmd, &dbResult, &nRow, 
        &nColumn, &errmsg) == SQLITE_OK)
    {
        offset = nColumn; //字段值从offset开始呀
        for( i = 0; i < nRow ; i++ )
        {
            if(dbResult[offset] != NULL)
            {
                strncpy(g_rnode[i+1].tox_id,dbResult[offset],TOX_ID_STR_LEN);
                f_num = check_and_add_friends(g_daemon_tox.ptox_handle,g_rnode[i+1].tox_id,g_daemon_tox.userinfo_fullurl);
                if(f_num < 0)
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO, "check add friend(%s) failed",g_rnode[i+1].tox_id);
                    g_rnode[i+1].c_status = PNR_RID_NODE_CSTATUS_CONNETERR;
                    g_rnode[i+1].f_id = f_num;
                }
                else
                {
                    DEBUG_PRINT(DEBUG_LEVEL_INFO, "check add friend(%s) OK",g_rnode[i+1].tox_id);
                    g_rnode[i+1].f_id = f_num;
                    g_rnode[i+1].c_status = PNR_RID_NODE_CSTATUS_CONNETTING;
                }
            }
            if(dbResult[offset+1] != NULL)
            {
                strncpy(g_rnode[i+1].node_name,dbResult[offset+1],PNR_USERNAME_MAXLEN);
            }
            offset += nColumn;
            if(i >= PNR_IMUSER_MAXNUM)
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR,"bad rnode num over");
                break;
            }
        }
        sqlite3_free_table(dbResult);
    }
    else
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"sqlite cmd(%s) err(%s)",sql_cmd,errmsg);
        sqlite3_free(errmsg);
        return ERROR;
    }
    return OK;
}

/**********************************************************************************
  Function:      pnr_monitor_warning_task
  Description:   设备监控告警
  Calls:
  Called By:
  Input:
  Output:        none
  Return:        0:成功
                 1:失败
  Others:

  History: 1. Date:2018-07-30
                  Author:Will.Cao
                  Modification:Initialize
***********************************************************************************/ 
static void *pnr_monitor_warning_task(void *para)
{
    struct pnr_monitor_errinfo* pinfo = (struct pnr_monitor_errinfo*)para;
    char* post_data = NULL;
    char* rec_buff = NULL;
    int data_len = 0;
    cJSON * params = NULL;
    if(pinfo == NULL)
    {
        return NULL;
    }
    
    rec_buff = malloc(BUF_LINE_MAX_LEN+1);
    if(rec_buff == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"rec_buff malloc err");
        return NULL;
    }
    memset(rec_buff,0,BUF_LINE_MAX_LEN);
    params = cJSON_CreateObject();
    if(params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"json create err");
        free(rec_buff);
        return NULL;
    }
    cJSON_AddItemToObject(params, "Dev", cJSON_CreateNumber(pinfo->dev_num));
    cJSON_AddItemToObject(params, "Mode", cJSON_CreateNumber(pinfo->mode_num));
    cJSON_AddItemToObject(params, "Errno", cJSON_CreateNumber(pinfo->err_no));
    cJSON_AddItemToObject(params, "Rid", cJSON_CreateString(pinfo->tox_id));
    cJSON_AddItemToObject(params, "Mac", cJSON_CreateString(pinfo->mac));
    cJSON_AddItemToObject(params, "Warn", cJSON_CreateString(pinfo->err_info));
    cJSON_AddItemToObject(params, "Info", cJSON_CreateString(pinfo->repair_info));
    post_data = cJSON_PrintUnformatted_noescape(params);
    cJSON_Delete(params);
    data_len = strlen(post_data);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_monitor_warning_task:post_data(%s)",post_data);
    https_post(PAPUSHMSG_PRODUCT_HTTPS_SERVER,PAPUSHMSG_HTTPSSERVER_PORT,PNR_HTTPSSERVER_DEVRWARN,post_data,data_len,rec_buff,BUF_LINE_MAX_LEN);
    free(post_data);
    params = cJSON_CreateObject();
    if(params == NULL)
    {
        DEBUG_PRINT(DEBUG_LEVEL_ERROR,"json create err");
        free(rec_buff);
        return NULL;
    }
    cJSON_AddItemToObject(params, "replay", cJSON_CreateString(rec_buff));
    post_data = cJSON_PrintUnformatted_noescape(params);
    cJSON_Delete(params);
    DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_monitor_warning_task:rec(%s)",post_data);
    free(post_data);
    free(rec_buff);
    return NULL;
}
/*****************************************************************************
 函 数 名  : pnr_netstat_check
 功能描述  : 检测网络阻塞
 输入参数  : 
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_netstat_check(struct pnr_monitor_errinfo* pinfo)
{
    FILE *fp = NULL;
    char* ptmp = NULL;
    int recq_num = 0;
    char buff[CMD_MAXLEN+1] = {0};
    char pcmd[CMD_MAXLEN+1] = {0};
    pthread_t task_id = 0;
    void *status;
    int pid = 0;
    char netstat_process[PNR_USERNAME_MAXLEN+1] = {0};
    if(pinfo == NULL)
    {
        return ERROR;
    }
    snprintf(pcmd,CMD_MAXLEN,"netstat -nlp |grep '*' |awk '{print $2,$6,$7}'");
    if (!(fp = popen(pcmd, "r"))) 
    {
        return ERROR;
    }

    while(NULL != fgets(buff, CMD_MAXLEN, fp)) 
    {
        recq_num = atoi(buff);
        ptmp = buff;
        if(buff[strlen(buff)-1] == '\n')
        {
            buff[strlen(buff)-1] = 0;
        }
        //DEBUG_PRINT(DEBUG_LEVEL_INFO,"get buff(%s)",buff);
        ptmp = strchr(buff,' ');
        if(ptmp != NULL)
        {
            ptmp++;
            if(strncasecmp(ptmp,"LISTEN",strlen("LISTEN")) == OK)
            {
                ptmp += strlen("LISTEN");
                ptmp++;
            }
            strncpy(netstat_process,ptmp,PNR_USERNAME_MAXLEN);
        }
        if(recq_num > PNR_NETSTAT_RECQMAXNUM)
        {
            //发现拥塞
            pinfo->err_no = PNR_MONITORINFO_ENUM_NETSTATERR;
            DEBUG_PRINT(DEBUG_LEVEL_INFO,"break");
            break;
        }
        //DEBUG_PRINT(DEBUG_LEVEL_INFO,"process(%s) recq(%d)",netstat_process,recq_num);
        memset(buff,0,CMD_MAXLEN);
        memset(netstat_process,0,PNR_USERNAME_MAXLEN);
    }
    pclose(fp);
    //如果发现拥塞，先上报，再处理
    if(pinfo->err_no == PNR_MONITORINFO_ENUM_NETSTATERR)
    {
        memset(pcmd,0,CMD_MAXLEN);
        snprintf(pinfo->err_info,PNR_ATTACH_INFO_MAXLEN,"process(%s) netstat recq %d",netstat_process,recq_num);
        if(strstr(netstat_process,PNR_NETSTAT_ERRPROCESS_RNGD) != NULL)
        {
            pid = atoi(netstat_process);
            if(pid > 0)
            {
                snprintf(pcmd,CMD_MAXLEN,"kill -9 %d & > /tmp/null",pid);
            }
            snprintf(pinfo->repair_info,PNR_ATTACH_INFO_MAXLEN,"kill process %s",netstat_process);
        }
        else if(strstr(netstat_process,PNR_NETSTAT_ERRPROCESS_PNRSERVER) != NULL 
            || strstr(netstat_process,PNR_NETSTAT_ERRPROCESS_FRPC) != NULL)
        {
            snprintf(pinfo->repair_info,PNR_ATTACH_INFO_MAXLEN,"restart pnr_server");
            snprintf(pcmd,CMD_MAXLEN,"killall pnr_server frpc > /tmp/null &");
        }
        else
        {
            snprintf(pinfo->repair_info,PNR_ATTACH_INFO_MAXLEN,"just warnning");
        }
        //上报
        if (pthread_create(&task_id, NULL, pnr_monitor_warning_task, pinfo) != 0) 
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pthread_create pnr_monitor_warning_task failed");
        }
        pthread_join(task_id,&status);
        pinfo->err_no = PNR_MONITORINFO_ENUM_OK;
        if(strlen(pcmd) > 0)
        {
            system(pcmd);
        }
    }
    return OK;
}
/*****************************************************************************
 函 数 名  : pnr_sysresource_check
 功能描述  : 检测系统资源
 输入参数  : 
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
int pnr_sysresource_check(struct pnr_monitor_errinfo* pinfo)
{
    FILE *fp = NULL;
    int tmp_used = 0;
    int cpu_used = 0;
    char buff[CMD_MAXLEN+1] = {0};
    char pcmd[CMD_MAXLEN+1] = {0};
    pthread_t task_id = 0;
    void *status;
    if(pinfo == NULL)
    {
        return ERROR;
    }
    if(g_pnrdevtype == PNR_DEV_TYPE_ONESPACE)
    {
        //检测tmp目录是否满
        snprintf(pcmd,CMD_MAXLEN,"df -h |grep /tmp |awk '{print $5}'");
        if (!(fp = popen(pcmd, "r"))) 
        {
            return ERROR;
        }
        if (fgets(buff,CMD_MAXLEN,fp) <= 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_sysresource_check cmd =%s ret failed",buff);
            pclose(fp);
            return ERROR;
        }  
        pclose(fp); 
        tmp_used = atoi(buff);
        if(tmp_used > PNR_SYSSOURCE_TMPBUFF_MAXNUM)
        {
            pinfo->err_no = PNR_MONITORINFO_ENUM_TMPOVER;
            snprintf(pinfo->err_info,PNR_ATTACH_INFO_MAXLEN,"tmp buff over %d",tmp_used);
            snprintf(pinfo->repair_info,PNR_ATTACH_INFO_MAXLEN,"kill onecgi");
            //上报
            if (pthread_create(&task_id, NULL, pnr_monitor_warning_task, pinfo) != 0) 
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pthread_create pnr_monitor_warning_task failed");
            }
            pthread_join(task_id,&status);
            system("/opt/go/onecgi.sh stop &");
            system("rm -f /tmp/oneapi.log");
            pinfo->err_no = PNR_MONITORINFO_ENUM_OK;
            memset(pinfo->err_info,0,PNR_ATTACH_INFO_MAXLEN);
            memset(pinfo->repair_info,0,PNR_ATTACH_INFO_MAXLEN);
        }
        //检测cpu是否过高
        memset(pcmd,0,CMD_MAXLEN);
        snprintf(pcmd,CMD_MAXLEN,"ps aux|grep pnr_server |grep -v grep |awk '{print $3}'");
        if (!(fp = popen(pcmd, "r"))) 
        {
            return ERROR;
        }
        if (fgets(buff,CMD_MAXLEN,fp) <= 0)
        {
            DEBUG_PRINT(DEBUG_LEVEL_ERROR,"pnr_sysresource_check cmd =%s ret failed",buff);
            pclose(fp);
            return ERROR;
        }  
        pclose(fp); 
        cpu_used = atoi(buff);
        if(cpu_used > PNR_SYSSOURCE_CPUINFO_MAXNUM)
        {
            pinfo->err_no = PNR_MONITORINFO_ENUM_CPUOVER;
            snprintf(pinfo->err_info,PNR_ATTACH_INFO_MAXLEN,"cpu over %d",cpu_used);
            snprintf(pinfo->repair_info,PNR_ATTACH_INFO_MAXLEN,"kill pnr_server");
            //上报
            if (pthread_create(&task_id, NULL, pnr_monitor_warning_task, pinfo) != 0) 
            {
                DEBUG_PRINT(DEBUG_LEVEL_ERROR, "pthread_create pnr_monitor_warning_task failed");
            }
            pthread_join(task_id,&status);            
            pinfo->err_no = PNR_MONITORINFO_ENUM_OK;
            memset(pinfo->err_info,0,PNR_ATTACH_INFO_MAXLEN);
            memset(pinfo->repair_info,0,PNR_ATTACH_INFO_MAXLEN);
            system("killall pnr_server frpc > /tmp/null &");
        }
        DEBUG_PRINT(DEBUG_LEVEL_INFO,"pnr_sysresource_check: tmp_used(%d) cpu_used(%d)",tmp_used,cpu_used);
    }
    return OK;
}
extern char g_dev_hwaddr[MACSTR_MAX_LEN];
/*****************************************************************************
 函 数 名  : self_monitor_thread
 功能描述  : 自我监测任务
 输入参数  : void arg  
 输出参数  : 无
 返 回 值  : 
 调用函数  : 
 被调函数  : 
 
 修改历史      :
  1.日    期   : 2018年11月30日
    作    者   : willcao
    修改内容   : 新生成函数

*****************************************************************************/
void *self_monitor_thread(void *para)
{	
    struct pnr_monitor_errinfo info;
    memset(&info,0,sizeof(info));
    while(g_p2pnet_init_flag != TRUE)
    {
        sleep(1);
    }
    strcpy(info.tox_id,g_daemon_tox.user_toxid);
    strcpy(info.mac,g_dev_hwaddr);
    info.dev_num = g_pnrdevtype;
    info.mode_num = 1;
	while (TRUE) 
    {
        //检测网络端口阻塞
        pnr_netstat_check(&info);
        pnr_sysresource_check(&info);
        sleep(PNR_SELF_MONITOR_CYCLE);
    }
	return NULL;  
}

