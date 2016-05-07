/*
 * Provides self-upgrade of UVP Tools on VMs.
 *
 * Copyright 2016, Huawei Tech. Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */


#include "libxenctl.h"
#include "public_common.h"
#include <sys/vfs.h>
#include "securec.h"
#include "uvpmon.h"

#define BUFFER_SIZE 1024
#define SHELL_BUFFER 256

#define UPGRADE_SUCCESS 0
#define MONITOR_UPGRADE_OK 3
#define UPGRADE_ROLLBACK 5

/* ISO�ļ� mountʧ�����Դ���*/
#define MOUNT_FAIL_RETRY 5

/* dom0��xenstoreע������ISO�ļ��ļ�ֵ */
#define UVP_FTPFAIL_PATH "control/uvp/upgrade/ftpfail"
#define UVP_MOUNTISO_PATH "control/uvp/upgrade/mountiso"

/* ����ͨ��������ֵ */
#define MOUNT_PATH "/mnt/pvmount"
#define COPY_FILE_PATH "/mnt/pvmount/cpfile.sh"
#define CONFIG_FILE "/mnt/pvmount/UpgradeInfo.ini"
#define DIR_TOOLS_TMP "/tmp/uvptools_temp/"
#define BIN_LIST_FILE "/tmp/uvptools_temp/tmp_bin_list"
#define UPGRADE_RESULT_FILE "/tmp/uvptools_temp/tmp_result"
#define TMP_RESULT_FILE "/var/run/uvp_tmp_result"
#define UVP_TIP_MESSAGE "control/uvp/domutray/tipinfo"
#define UVP_CHANNEL_RESULT_PATH "control/uvp/upgrade/result/channel"
#define FILE_OS_TYPE "/etc/.uvp-monitor/ostype.ini"
#define MIN_SPACE 60

#define UVP_UPGRADE_FLAG_PATH "control/uvp/upgrade_flag"
#define UVP_UPGRADE_RESULT_PATH "control/uvp/upgrade/result"
#define UVP_HOTMIGRATE_FLAG_PATH "control/uvp/migrate_flag"

#define UVP_UPGRADE_STRATEGY_PATH "control/uvp/upgrade/upgrade_strategy"

// declared in xenctlmon.c
extern int g_need_reboot_after_upgrade;
extern char fReboot;

/*****************************************************************************
Function   : trim
Description: ȥ���ִ�β���ո�
Input      : char *
Output     : None
Return     : char *
*****************************************************************************/
char *trim(char *str)
{
    char *p = NULL;

    if(NULL == str)
    {
    	return NULL;
    }

    p = str + strlen(str) - 1;
    while(' ' == *p || '\t' == *p || '\n' == *p || '\r' == *p)
    {
        *p = '\0';
        p--;
    }
    return str;
}
/*****************************************************************************
Function   : getfreedisk
Description: ��ȡʣ��ռ�
Input       :path Ŀ¼·����
Output     : None
Return     :  long
*****************************************************************************/
long getfreedisk(char *path)
{
    struct statfs diskstat;
    (void)memset_s(&diskstat, sizeof(diskstat), 0, sizeof(diskstat));
    (void)statfs(path, &diskstat);
    return (long long)diskstat.f_bsize * (long long)diskstat.f_bavail / 1024 / 1024;
}
/*****************************************************************************
Function   : exe_command
Description: ʹ��systemִ������
Input       :�������ݻ�·��
Output     : None
Return     : int
*****************************************************************************/
int exe_command(char *path)
{
    char logBuf[BUFFER_SIZE] = {0};
    int flag,exit_value;
    exit_value = system(path);
    flag = WEXITSTATUS(exit_value);
	//flag = 1,3,5��ʱ����Ҫ��ʾ����
    if ( (UPGRADE_SUCCESS != flag) && (MONITOR_UPGRADE_OK != flag) && (UPGRADE_ROLLBACK != flag) )
    {
        (void)snprintf_s(logBuf, BUFFER_SIZE - 1, BUFFER_SIZE - 1, "exe error-command:%s", path);
        INFO_LOG("[Monitor-Upgrade]: logBuf %s",logBuf);
        return 1;
    }

    if(UPGRADE_ROLLBACK == flag)
    {
        return 5;
    }

    if(MONITOR_UPGRADE_OK == flag)
	{
		return 3;
	}

    return 0;
}
/*****************************************************************************
Function   : check_upg
Description: ����Ƿ���Կ�ʼ����
Input       :handle : xenstore�ľ��
Output     : None
Return     : int
*****************************************************************************/
int check_upg(void *handle)
{
    int count = 0;
    long diskspace = 0;

    while (count < 30)
    {
    	(void)sleep(1);
        if ( 0 == access("/dev/xvdd", R_OK) )
        {
            break;
        }
        else
        {
            ERR_LOG("[Monitor-Upgrade]: access /dev/xvdd R_ERROR");
            count++;
            continue;
        }
    }

    if ( 30 == count)
    {
    	ERR_LOG("[Monitor-Upgrade]: check /dev/xvdd timeout");
        (void)write_to_xenstore(handle, UVP_CHANNEL_RESULT_PATH, "failed:channel:mountiso-fail");
        return 1;
    }

    diskspace = getfreedisk("/tmp");
    if(diskspace < MIN_SPACE)
    {
        (void)write_to_xenstore(handle, UVP_CHANNEL_RESULT_PATH, "failed:channel:no-space");
        DEBUG_LOG("[Monitor-Upgrade]: no-space!");
        return 1;
    }

    return 0;

}

int is_debian()
{
    FILE *pF = NULL;
    char strIssue[SHELL_BUFFER] = {0};
    pF = fopen("/etc/issue", "r");
    if (NULL == pF)
    {
        INFO_LOG("[Monitor-Upgrade]: open /etc/issue fail.");
        return 1;
    }

    while(fgets(strIssue, SHELL_BUFFER-1, pF) != NULL)
    {
        if (strstr(strIssue, "Debian GNU/Linux"))
        {
            fclose(pF);
            return 0;
        }
    }

    fclose(pF);
    return 1;
}

/*****************************************************************************
Function   : clean_tmp_files
Description: �����쳣ʱ������ʱ�ļ�
Input      :
Output     : None
Return     : None
*****************************************************************************/
void clean_tmp_files()
{
    char cleanTmpBuf[SHELL_BUFFER] = {0};
    (void)memset_s(cleanTmpBuf, SHELL_BUFFER, 0, SHELL_BUFFER);
    (void)snprintf_s(cleanTmpBuf, SHELL_BUFFER, SHELL_BUFFER, "rm -rf %s 2> /dev/null;", DIR_TOOLS_TMP);
    (void)exe_command(cleanTmpBuf);
}

/*****************************************************************************
Function   : roll_back_iso
Description: ֪ͨж��iso
Input       :handle : xenstore�ľ��
Output     : None
Return     : None
*****************************************************************************/
void roll_back_iso(void *handle)
{
    char mountIsoBuf[SHELL_BUFFER] = {0};
    (void)memset_s(mountIsoBuf, SHELL_BUFFER, 0, SHELL_BUFFER);
    (void)snprintf_s(mountIsoBuf, SHELL_BUFFER, SHELL_BUFFER,
                    "umount -f %s 2> /dev/null;rm -rf %s;", MOUNT_PATH, MOUNT_PATH);
    (void)exe_command(mountIsoBuf);
    (void)write_to_xenstore(handle, UVP_UPGRADE_FLAG_PATH, "0");
    write_to_xenstore(handle, UVP_FTPFAIL_PATH, "tools-upgrade-ok");
}
/*****************************************************************************
Function   : write_to_binfile
Description: д��bin��ʱ�ļ�
Input       :handle : xenstore�ľ��
Output     : None
Return     : int
*****************************************************************************/
int write_to_binfile(void *handle, char *binBuf)
{
    FILE *fbin = NULL;
    char buf[SHELL_BUFFER] = {0};
    char *start = NULL;

    fbin = fopen(BIN_LIST_FILE, "a+t");
    if (NULL == fbin)
    {
        INFO_LOG("[Monitor-Upgrade]: open BIN_LIST_FILE failed.");
        roll_back_iso(handle);
        return 1;
    }
    rewind(fbin);

    while(NULL != fgets(buf, SHELL_BUFFER, fbin))
    {
        start = strstr(buf, binBuf);
        if (NULL != start)
        {
            fclose(fbin);
            return 0;
        }

    }
    (void)fputs(binBuf, fbin);
    (void)fclose(fbin);
    return 0;
}
/*****************************************************************************
Function   : write_tools_result
Description: �������д��xenstore
Input       :handle : xenstore�ľ��
Output     : None
Return     : int
*****************************************************************************/
void write_tools_result(void *handle)
{
    char buf[SHELL_BUFFER] = {0};
    char moduleBuf[SHELL_BUFFER] = {0};
    char resultBuf[SHELL_BUFFER] = {0};
    FILE *fResult = NULL;
    char *start = NULL;
    char *end = NULL;

    if( 0 == access(UPGRADE_RESULT_FILE, R_OK))
    {
        fResult = fopen(UPGRADE_RESULT_FILE, "r+t");
    }
    else
    {
        fResult = fopen(TMP_RESULT_FILE, "r+t");
    }

    if (NULL == fResult)
    {
        INFO_LOG("[Monitor-Upgrade]: failed to open UPGRADE_RESULT_FILE\n");
        return;
    }
    while(NULL != fgets(buf, SHELL_BUFFER - 1, fResult))
    {
        if ( strlen(buf) <= 0)
        {
            INFO_LOG("[Monitor-Upgrade]: null line\n");
            continue;
        }
        start = strstr(buf, ":");
        if (NULL == start)
        {
            INFO_LOG("[Monitor-Upgrade]: format of result is wrong\n");
            continue;
        }

        start = start + strlen(":");
        end = strstr(start, ":");

        if (NULL == end)
        {
            INFO_LOG("[Monitor-Upgrade]: format of result is wrong\n");
            continue;
        }
        (void)memset_s(moduleBuf, SHELL_BUFFER, 0, SHELL_BUFFER);
        memcpy_s(moduleBuf, SHELL_BUFFER, start, end - start);
        (void)snprintf_s(resultBuf, SHELL_BUFFER, SHELL_BUFFER, "%s/%s", UVP_UPGRADE_RESULT_PATH, moduleBuf);

        (void)write_to_xenstore(handle, resultBuf, trim(buf));
    }
    fclose(fResult);
}
/*****************************************************************************
Function   : read_config_file
Description: ��ȡ�����ļ�
Input       :handle : xenstore�ľ��
Output     : None
Return     : None
*****************************************************************************/
int read_config_file(void *handle, FILE *fp, char *moduleBuf)
{
    char *start = NULL;
    char buf[SHELL_BUFFER] = {0};
    char exeBuf[SHELL_BUFFER] = {0};
    int ret;

    while(NULL != fgets(buf, SHELL_BUFFER - 1, fp))
    {
        start = strstr(buf, "binFile=");
        if (NULL != start)
        {
            start = start + strlen("binFile=");
            (void)memset_s(exeBuf, SHELL_BUFFER, 0, SHELL_BUFFER);
            (void)snprintf_s(exeBuf, SHELL_BUFFER - 1, SHELL_BUFFER - 1, "%s%s/%s", DIR_TOOLS_TMP, moduleBuf, start);
            ret = write_to_binfile(handle, exeBuf);
            if (1 == ret)
            {
                return 1;
            }
            continue;
        }
        start = strstr(buf, "isNeedReboot=");
        if (NULL != start)
        {
            if ('1' == start[strlen("isNeedReboot=")])
            {
                fReboot = '1';
            }
            continue;
        }
    }

    return 0;
}
/*****************************************************************************
Function   : do_tools_up
Description: ��ȡ�����ļ�
Input       :handle : xenstore�ľ��
Output     : None
Return     : None
*****************************************************************************/
void do_tools_up(void *handle)
{

    FILE *fbin = NULL;
    char buf[SHELL_BUFFER] = {0};
    char mountIsoBuf[BUFFER_SIZE] = {0};
    int pvFlag = 0;
    int exeFlag = 0;
    int ret = 0;
    char *start = NULL;

	//֪ͨ���̵���������ʾ�û���������ĪҪ�ػ�
    (void)write_to_xenstore(handle, UVP_TIP_MESSAGE, "start-upgrade");
    //ִ������ű�����дִ�еĽ��
    fbin = fopen(BIN_LIST_FILE, "r+t");
    if (NULL == fbin)
    {
        INFO_LOG("[Monitor-Upgrade]: open BIN_LIST_FILE R_ERROR when to read!");
        (void)write_to_xenstore(handle, UVP_TIP_MESSAGE, "over-upgrade");
        return;
    }
    (void)memset_s(buf, SHELL_BUFFER, 0, SHELL_BUFFER);

    while (NULL != fgets(buf, SHELL_BUFFER - 1, fbin) && buf[0] != '\0')
    {
        start = strstr(buf, "pvdriver");
        (void)trim(buf);
        (void)memset_s(mountIsoBuf, BUFFER_SIZE, 0, BUFFER_SIZE);
        (void)snprintf_s(mountIsoBuf, BUFFER_SIZE, BUFFER_SIZE,
                        "dos2unix %s 1> /dev/null 2>&1;chmod +x %s 2> /dev/null;", buf, buf);
        (void)exe_command(mountIsoBuf);
        ret = exe_command(buf);

        if (UPGRADE_SUCCESS == ret)
        {
            exeFlag = 1;
        }

	if (NULL != start && (UPGRADE_SUCCESS == ret || MONITOR_UPGRADE_OK == ret || UPGRADE_ROLLBACK == ret))
	{
            pvFlag = 1;
        }

	INFO_LOG("[Monitor-Upgrade]: Execution %s",buf);
    }

    if (0 == exeFlag)
    {
        fReboot = '0';
    }

    (void)fclose(fbin);

    write_tools_result(handle);

    (void)memset_s(mountIsoBuf, BUFFER_SIZE, 0, BUFFER_SIZE);
    (void)snprintf_s(mountIsoBuf, BUFFER_SIZE, BUFFER_SIZE,
                    "rm -rf %s 2> /dev/null;cp -f %s %s 2> /dev/null;",
                    TMP_RESULT_FILE, UPGRADE_RESULT_FILE, TMP_RESULT_FILE);
    (void)exe_command(mountIsoBuf);
    /* upgrade message */
    if(MONITOR_UPGRADE_OK == ret)
    {
       (void)write_to_xenstore(handle, UVP_TIP_MESSAGE, "monitor-ok");
    }
    else
    {
       //����fReboot���жϵ����������ǽ���������
       if ( '1' == fReboot  )
       {
          (void)write_to_xenstore(handle, UVP_TIP_MESSAGE, "end-upgrade");
       }
       else
       {
          (void)write_to_xenstore(handle, UVP_TIP_MESSAGE, "over-upgrade");
       }
    }
    fReboot = '0';
    /* clean tmp */
    (void)memset_s(mountIsoBuf, BUFFER_SIZE, 0, BUFFER_SIZE);
    (void)snprintf_s(mountIsoBuf, BUFFER_SIZE, BUFFER_SIZE, "rm -rf %s 2> /dev/null;", DIR_TOOLS_TMP);
    (void)exe_command(mountIsoBuf);

    (void)sleep(5);
    (void)write_to_xenstore(handle, UVP_TIP_MESSAGE, "uvptoken");
    (void)write_to_xenstore(handle, UVP_MOUNTISO_PATH, "uvptoken");

    if(pvFlag)
    {
    	INFO_LOG("[Monitor-Upgrade]: reboot uvp-monitor");
    	INFO_LOG("[Monitor-Upgrade]: unwatch regwatch");
		g_monitor_restart_value = 1;
        uvp_unregwatch(handle);
        if( 0 == access("/etc/init.d/uvp-monitor", R_OK))
        {
            (void)exe_command("/etc/init.d/uvp-monitor restart 1>/dev/null 2>&1");
        }
        else
        {
            (void)exe_command("/etc/init.d/monitor restart 1>/dev/null 2>&1");
        }
    }
}
/*****************************************************************************
Function   : doMountISOUpgrade
Description: ����watch�¼�
Input       :handle : xenstore�ľ��
Output     : None
Return     : None
*****************************************************************************/
void doMountISOUpgrade(void *handle)
{
    char mountIsoBuf[BUFFER_SIZE] = {0};
    char *pathValue = NULL;
    char *start = NULL;
    char *end = NULL;
    int ret = 0;
    char buf[SHELL_BUFFER] = {0};
    FILE *fcon = NULL;
    char pathBuf[SHELL_BUFFER] = {0};
    char moduleBuf[SHELL_BUFFER] = {0};
    char xenBuf[SHELL_BUFFER] = {0};
    char failedBuf[SHELL_BUFFER] = {0};
    char *module = moduleBuf;
    int retry_cnt = 0;

    pathValue = read_from_xenstore(handle, UVP_MOUNTISO_PATH);
    if ( NULL == pathValue || (pathValue != NULL && pathValue[0] == '\0') )
    {
	/*added by h00241659 on 2013-5-17*/
	if (NULL != pathValue)
	{
		free(pathValue);
		pathValue = NULL;
	}
	/*added end*/
        return;
    }

	//mount-tools-iso��ֵ����ISO�Ѿ����ص������
    if (!strcmp(pathValue, "mount-tools-iso"))
    {
        if (NULL != pathValue)
        {
            free(pathValue);
            //lint -save -e438
            pathValue = NULL;
            //lint -restore
        }
    	INFO_LOG("[Monitor-Upgrade]: start mount-tools-iso");
        if(0 == access("/tmp/pv_temp_file", R_OK))
        {
            INFO_LOG("[Monitor-Upgrade]: In test mod, will roll back iso.");
            (void)memset_s(mountIsoBuf, BUFFER_SIZE, 0, BUFFER_SIZE);
            (void)snprintf_s(mountIsoBuf, BUFFER_SIZE, BUFFER_SIZE,
                            "rm -f /tmp/pv_temp_file 2> /dev/null;");
            ret = exe_command(mountIsoBuf);
            if (1 == ret)
            {
                (void)write_to_xenstore(handle, UVP_CHANNEL_RESULT_PATH, "failed:pvdriver:copyiso-fail");
            }
            roll_back_iso(handle);

            return;
        }

        /* ��������ʱĿ¼ */
        clean_tmp_files();
    	//�ж�������Ƿ������������
        ret = check_upg(handle);
        if (1 == ret)
        {
            ERR_LOG("[Monitor-Upgrade]: check_upg failed");
            write_to_xenstore(handle, UVP_FTPFAIL_PATH, "tools-upgrade-ok");
            (void)write_to_xenstore(handle, UVP_UPGRADE_FLAG_PATH, "0");
            return;
        }

	    INFO_LOG("[Monitor-Upgrade]: check_upg ok");

        (void)memset_s(mountIsoBuf, BUFFER_SIZE, 0, BUFFER_SIZE);
    	if(0 == access("/proc/xen/version", R_OK))
    	{
                (void)sleep(10);
                INFO_LOG("[Monitor-Upgrade]: PVOPS Kernel");
                (void)snprintf_s(mountIsoBuf, BUFFER_SIZE, BUFFER_SIZE,
                    "umount -f /dev/xvdd 2> /dev/null;umount -f %s 2> /dev/null;rm -rf %s;mkdir -p %s 2> /dev/null;mount -t iso9660 /dev/xvdd %s 2> /dev/null;",
                    MOUNT_PATH, MOUNT_PATH, MOUNT_PATH, MOUNT_PATH);
    	}
    	else
    	{
                (void)snprintf_s(mountIsoBuf, BUFFER_SIZE, BUFFER_SIZE,
                    "umount -f /dev/xvdd 2> /dev/null;umount -f %s 2> /dev/null;rm -rf %s;mkdir -p %s 2> /dev/null;mount /dev/xvdd %s 2> /dev/null;",
                    MOUNT_PATH, MOUNT_PATH, MOUNT_PATH, MOUNT_PATH);
    	}

	    INFO_LOG("[Monitor-Upgrade]: check pvops ok");

        retry_cnt = MOUNT_FAIL_RETRY;
        do {
            ret = exe_command(mountIsoBuf);
            if (1 != ret)
            {
                break;
            }

            if (retry_cnt)
            {
                /* In our test, udevd will retry cdrom_id cmd in 4s if failure.
                 * So, retry mount in 5s should be fine for most case.
                 */
                INFO_LOG("[Monitor-Upgrade]: mount cdrom failed, will retry in 5s");
                (void)sleep(5);
                continue;
            }

            (void)write_to_xenstore(handle, UVP_CHANNEL_RESULT_PATH, "failed:channel:mountiso-fail");
            (void)write_to_xenstore(handle, UVP_UPGRADE_FLAG_PATH, "0");
            write_to_xenstore(handle, UVP_FTPFAIL_PATH, "tools-upgrade-ok");
            return;
        } while (retry_cnt--);

	    INFO_LOG("[Monitor-Upgrade]: mount cdrom ok");

        if (0 == is_debian())
        {
            (void)sleep(10);
            (void)memset_s(mountIsoBuf, BUFFER_SIZE, 0, BUFFER_SIZE);
            (void)snprintf_s(mountIsoBuf, BUFFER_SIZE, BUFFER_SIZE, "mount /dev/xvdd %s 2> /dev/null;",  MOUNT_PATH);
            (void)exe_command(mountIsoBuf);
            INFO_LOG("[Monitor-Upgrade]: debian linux, try again");
        }

        if(0 == access("/proc/xen/version", R_OK))
        {
            (void)sleep(3);
        }

        INFO_LOG("[Monitor-Upgrade]: start check UpgradeInfo.ini");
        if ( access(CONFIG_FILE, R_OK) != 0 )
        {
            (void)write_to_xenstore(handle, UVP_CHANNEL_RESULT_PATH, "failed:channel:no-upgrade-info");
            INFO_LOG("[Monitor-Upgrade]: no-upgrade-info!");
            roll_back_iso(handle);
            return;
        }

	    INFO_LOG("[Monitor-Upgrade]: check UpgradeInfo.ini ok");

        fcon = fopen(CONFIG_FILE, "r");
        if (NULL == fcon)
        {
            INFO_LOG("[Monitor-Upgrade]: open CONFIG_FILE R_ERROR");
            roll_back_iso(handle);
            return;
        }
        /* ���������ļ������Ƚ���ģ����*/
        (void)fgets(buf, SHELL_BUFFER - 1, fcon);
        start = strstr(buf, "[");
        if (NULL == start)
        {
            (void)write_to_xenstore(handle, UVP_CHANNEL_RESULT_PATH, "failed:channel:no-module-name");
            INFO_LOG("[Monitor-Upgrade]: get ModuleName error");
            roll_back_iso(handle);
            fclose(fcon);
            return;
        }
        start = start + strlen("[");
        end = strstr(start, "]");
        if (NULL == end)
        {
            (void)write_to_xenstore(handle, UVP_CHANNEL_RESULT_PATH, "failed:channel:no-module-name");
            INFO_LOG("[Monitor-Upgrade]: get ModuleName error");
            roll_back_iso(handle);
            fclose(fcon);
            return;
        }

        memcpy_s(moduleBuf, SHELL_BUFFER, start, end - start);

	    INFO_LOG("[Monitor-Upgrade]: upgrade module %s",moduleBuf);

        //mkdir copypv
        (void)snprintf_s(pathBuf, SHELL_BUFFER, SHELL_BUFFER, "%s%s", DIR_TOOLS_TMP, moduleBuf);
        (void)memset_s(mountIsoBuf, BUFFER_SIZE, 0, BUFFER_SIZE);
        (void)snprintf_s(mountIsoBuf, BUFFER_SIZE, BUFFER_SIZE,
                        "rm -rf %s 2> /dev/null;mkdir -p %s 2> /dev/null;", pathBuf, pathBuf);
        (void)exe_command(mountIsoBuf);

	    INFO_LOG("[Monitor-Upgrade]: creat dir %s ok",mountIsoBuf);

        (void)snprintf_s(xenBuf, SHELL_BUFFER, SHELL_BUFFER, "%s/%s", UVP_UPGRADE_RESULT_PATH, moduleBuf);

		//���������ļ��е�������Ϣ
        ret = read_config_file(handle, fcon, module);
	    if (ret == 1)
	    {
		    (void)fclose(fcon);
		    roll_back_iso(handle);
		    return;
	    }
        (void)fclose(fcon);

	    INFO_LOG("[Monitor-Upgrade]: read_config_file ok");

        /* ִ������Ŀ����ű�*/
        ret = exe_command(COPY_FILE_PATH);
        if (1 == ret)
        {
            (void)snprintf_s(failedBuf, SHELL_BUFFER, SHELL_BUFFER, "failed:%s:copyiso-fail", moduleBuf);
            (void)write_to_xenstore(handle, xenBuf, failedBuf);
            roll_back_iso(handle);
            clean_tmp_files();
            return;
        }

	    INFO_LOG("[Monitor-Upgrade]: copy isofile ok");

        roll_back_iso(handle);

    }
    else if(!strcmp(pathValue, "start_upgrade"))
    {
        if (NULL != pathValue)
        {
            free(pathValue);
            //lint -save -e438
            pathValue = NULL;
            //lint -restore
        }
	    INFO_LOG("[Monitor-Upgrade]: start start_upgrade");
        do_tools_up(handle);
    }
    if (NULL != pathValue)
    {
        free(pathValue);
        //lint -save -e438
        pathValue = NULL;
    }//lint -restore

}

/*****************************************************************************
Function   : DoUpgrade
Description: ����watch�¼������control/uvp/upgrade/mountiso
Input       :handle : xenstore�ľ�� , path : xenstore·��
Output     : None
Return     : None
*****************************************************************************/
void doUpgrade(void *handle, char *path)
{
	char *pathValue = NULL;
	/*���ж��Ƿ����Աģʽ�Ƿ����ֶ�������linuxϵͳ��
	����ԱģʽΪ�ֶ�����ʱ��ֱ�Ӳ�����*/
	pathValue = read_from_xenstore(handle, UVP_UPGRADE_STRATEGY_PATH);
	if ((NULL != pathValue) && (!strcmp(pathValue, "1")))
	{
            INFO_LOG("[Monitor-Upgrade]: Linux. admin-abort");
            //д��ʧ�ܴ�����
            (void)write_to_xenstore(handle, UVP_CHANNEL_RESULT_PATH, "failed:channel:admin-abort");
            //���������־λ
            (void)write_to_xenstore(handle, UVP_UPGRADE_FLAG_PATH, "0");
            //֪ͨ�����������ж�ع���
            (void)write_to_xenstore(handle, UVP_FTPFAIL_PATH, "tools-upgrade-ok");
            free(pathValue);
            pathValue = NULL;
            return ;
	}

	if (NULL != pathValue)
	{
            free(pathValue);
            pathValue = NULL;
	}

    //������sr0/hdd ���͵Ĺ�����ͳһ����xvdd��������
    if ( (access("/dev/sr0", R_OK) == 0) )
    {
        (void)exe_command("rm -rf /dev/xvdd 2> /dev/null; ln -sf /dev/sr0 /dev/xvdd 2> /dev/null;");
        if(0 == eject_command())
            (void)exe_command("eject /dev/sr0 2> /dev/null;eject /dev/sr0 2> /dev/null;");
        else
            (void)exe_command("umount -f /dev/sr0 2> /dev/null;umount -f /dev/sr0 2> /dev/null;");
    }
    else if ( (access("/dev/hdd", R_OK) == 0) )
    {
        (void)exe_command("rm -rf /dev/xvdd 2> /dev/null; ln -sf /dev/hdd /dev/xvdd 2> /dev/null;");
        if(0 == eject_command())
            (void)exe_command("eject /dev/hdd 2> /dev/null;eject /dev/hdd 2> /dev/null;");
        else
            (void)exe_command("umount -f /dev/hdd 2> /dev/null;umount -f /dev/hdd 2> /dev/null;");
    }

    INFO_LOG("[Monitor-Upgrade]: creat ln ok");
    if (NULL != strstr(path, UVP_MOUNTISO_PATH))
    {
    	//����mountiso��ֵ�¾������ֵ
        (void)doMountISOUpgrade(handle);
    }

    return;
}

