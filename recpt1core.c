#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include "recpt1core.h"
#include "version.h"
#include "pt1_dev.h"
#include <sys/poll.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>

#define ISDB_T_NODE_LIMIT 24        // 32:ARIB limit 24:program maximum
#define ISDB_T_SLOT_LIMIT 8

#ifndef DTV_STREAM_ID
#define DTV_STREAM_ID		DTV_ISDBS_TS_ID
#endif

#define DMX_BUFFER_SIZE 1024 * 1024

/* globals */
boolean f_exit = FALSE;
char name_buff[16];
ISDB_T_FREQ_CONV_TABLE isdb_t_conv_set = {
    .parm_freq = name_buff
};

// 機種依存用
#define OTHER_TUNER	0x0000		// 未知
#define ISDBS_TUNER	0x0001		// ISDB-S(衛星チューナー)フラグ
#define EARTH_PT1	0x0002		// PT1 PT2
#define EARTH_PT3	0x0004		// PT3 PX-BCUD(?)
#define SIANO_MBL	0x0008		// PX-S1UD S880
#define DIBCOM8000	0x0010		// S870
#define FRIIO_WT	0x0020		// Friio白
int tuner_type = OTHER_TUNER;
struct {
	char *name;
	int type;
} tuners_prop[] = {
	{ "VA1J5JF8007/VA1J5JF8011 ISDB-", EARTH_PT1 },
	{ "Toshiba TC90522 ISDB-", EARTH_PT3 },
	{ "774 Friio ISDB-T USB2.0", FRIIO_WT },
	{ "Siano Mobile Digital MDTV Receiver", SIANO_MBL },
	{ "DiBcom 8000 ISDB-T", DIBCOM8000},
	{ NULL, 0 }
};

static unsigned int GetFrequency_S(int ch)
{
	unsigned int freq;
	if (ch < 12)
		freq = 1049480 + 38360 * ch;
	else if (ch < 24)
		freq = 1613000 + 40000 * (ch - 12);
	else if (ch < 36)
		freq = 1593000 + 40000 * (ch - 24);
	else if (ch < 48)
		freq = 2241660 + 38360 * (ch - 36);
	else if (ch < 61)
		freq = 2726000 + 40000 * (ch - 48);
	else
		freq = 1049480;	// 変な値なので0chに
	return freq;
}

static unsigned int GetFrequency_T(int ch)
{
	unsigned int freq;
	if (ch < 12)
		freq = 93142857 + 6000000 * ch;
	else if (ch < 17)
		freq = 167142857 + 6000000 * (ch - 12);
	else if (ch < 63)
		freq = 195142857 + 6000000 * (ch - 17);
	else if (ch < 113)
		freq = 473142857 + 6000000 * (ch - 63);
	else
		freq = 93142857;	// 変な値なので0chに
	return freq;
}

/* lookup frequency conversion table*/
boolean
search_channelS(char *channel)
{
    unsigned int tsid = 0;
    unsigned int node = 0;
    unsigned int slot = 0;
    unsigned int network_id = 0;
    char *bs_ch = channel;
    int lp;

    if(channel[0] == '0' && ( channel[1] == 'X' || channel[1] == 'x')){
		tsid = strtol( channel, NULL, 16 );
		bs_ch += strlen( bs_ch );
	}else{
	    boolean bs_type = FALSE;

	    if(channel[0] == 'B' && channel[1] == 'S') {
	        bs_ch += 2;
	        bs_type = TRUE;
	    }
	    while(isdigit(*bs_ch)) {
	        tsid *= 10;
	        tsid += *bs_ch++ - '0';
	    }
	    if( bs_type ){
	        if(*bs_ch == '_' && (tsid&0x01) && tsid < ISDB_T_NODE_LIMIT) {
	            if(isdigit(*++bs_ch)) {
	                slot = *bs_ch - '0';
	                if(*++bs_ch == '\0' && slot < ISDB_T_SLOT_LIMIT) {
						node = tsid / 2;
	    				for(lp = 0; isdb_t_conv_table[lp].set_freq<12; lp++){
							if( isdb_t_conv_table[lp].set_freq == node && isdb_t_conv_table[lp].add_freq == slot ){
					            isdb_t_conv_set.set_freq = node;
					            isdb_t_conv_set.type = CHTYPE_SATELLITE;
			                    isdb_t_conv_set.add_freq = slot;
					            isdb_t_conv_set.tsid = isdb_t_conv_table[lp].tsid;
			                    sprintf(name_buff, "BS%d_%d", tsid, slot);
					            return TRUE;
					        }
					    }
	                }
	            }
	        }
	        return FALSE;
		}
	}
    if( 0x4010U<=tsid && tsid<=0xcfffU && *bs_ch=='\0' ){
		node = ( tsid & 0x01f0U ) >> 4;
        slot = tsid & 0x0007U;
        network_id = (tsid & 0xf000U) >> 12;
        if(network_id == 0x4 || network_id == 0xb){
			if( node & 0x0001 ){
	            isdb_t_conv_set.set_freq = node / 2;
                if( node == 15 )
                    slot--;
                isdb_t_conv_set.add_freq = slot;
	        } else {
	            isdb_t_conv_set.set_freq = node / 2 + 35;
            }
        }else{
			if( (node & 0x0001) == 0 ){
         	    isdb_t_conv_set.set_freq = node / 2 + 11;
                isdb_t_conv_set.add_freq = 0;
	        }else {
                if (node == 25) {
                    node = 0;
                } else {
                    node += 2;
                }
         	    isdb_t_conv_set.set_freq = node / 2 + 48;
                isdb_t_conv_set.add_freq = 0;
            }
        }
		isdb_t_conv_set.type = CHTYPE_SATELLITE;
        isdb_t_conv_set.tsid = tsid;
        sprintf(name_buff, "0x%X", tsid);
        return TRUE;
	}
    return FALSE;
}

ISDB_T_FREQ_CONV_TABLE *
searchrecoff(char *channel)
{
    int lp;

    if(search_channelS(channel))
		return &isdb_t_conv_set;
    for(lp = 0; isdb_t_conv_table[lp].parm_freq != NULL; lp++) {
        /* return entry number in the table when strings match and
         * lengths are same. */
        if((memcmp(isdb_t_conv_table[lp].parm_freq, channel,
                   strlen(channel)) == 0) &&
           (strlen(channel) == strlen(isdb_t_conv_table[lp].parm_freq))) {
            return &isdb_t_conv_table[lp];
        }
    }
    return NULL;
}

int
set_frequency(thread_data *tdata, boolean msg_view)
{
    struct dtv_property prop[4];
    struct dtv_properties props;
    struct dvb_frontend_info fe_info;
    int i;
    boolean isdb_t = false;
    boolean isdb_s = false;

    if( (ioctl(tdata->fefd,FE_GET_INFO, &fe_info) < 0)){
        fprintf(stderr, "FE_GET_INFO failed\n");
        return 1;
    }
    isdb_t = fe_info.type == FE_OFDM;
    isdb_s = fe_info.type == FE_QPSK;

    // 機種判別
    for(i = 0; tuners_prop[i].name!=NULL; i++){
	    if(strncmp(fe_info.name, tuners_prop[i].name, strlen(tuners_prop[i].name)) == 0)
	    	tuner_type = tuners_prop[i].type;
	}

    if (tuner_type == OTHER_TUNER) {
        prop[0].cmd = DTV_ENUM_DELSYS;
        props.num = 1;
        props.props = prop;
        if(ioctl(tdata->fefd, FE_GET_PROPERTY, &props) >= 0){
            for (i = 0; i < prop[0].u.buffer.len; i++) {
                if (prop[0].u.buffer.data[i] == SYS_ISDBT) {
                    isdb_t = TRUE;
                }
                if (prop[0].u.buffer.data[i] == SYS_ISDBS) {
                    isdb_s = TRUE;
                }
            }
        }
    }

    if(tdata->table->type == CHTYPE_GROUND){
        if(!isdb_t){
            if(msg_view)
                fprintf(stderr, "tuner does not support ISDB-T\n");
            return 1;
        }
        fprintf(stderr,"\nUsing DVB device \"%s\"\n",fe_info.name);

        prop[0].cmd = DTV_DELIVERY_SYSTEM;
        prop[0].u.data = SYS_ISDBT;
        prop[1].cmd = DTV_FREQUENCY;
        prop[1].u.data = GetFrequency_T(tdata->table->set_freq);
#if 0
	    prop[1].cmd = DTV_STREAM_ID;
        if(tuner_type != FRIIO_WT)
	        prop[1].u.data = 0;
        prop[2].cmd = DTV_TUNE;
        props.props = prop;
        props.num = 3;
#else
        prop[2].cmd = DTV_TUNE;
        props.props = prop;
        props.num = 3;
#endif
        fprintf(stderr,"tuning to %.3f MHz\n",(double)prop[1].u.data / 1000000);
    }else
    if(tdata->table->type == CHTYPE_SATELLITE){
        if(!isdb_s){
            if(msg_view)
                fprintf(stderr, "tuner does not support ISDB-S\n");
            return 1;
        }
        fprintf(stderr,"\nUsing DVB device \"%s\"\n",fe_info.name);
        tuner_type |= ISDBS_TUNER;

		if(0 <= tdata->lnb && tdata->lnb <= 2){
			prop[0].cmd = DTV_VOLTAGE;
			prop[0].u.data = tdata->lnb;
			props.props = prop;
			props.num = 1;
			if (ioctl(tdata->fefd, FE_SET_PROPERTY, &props) < 0)
				fprintf(stderr, "LNB control failed");
		}

        prop[0].cmd = DTV_DELIVERY_SYSTEM;
        prop[0].u.data = SYS_ISDBS;
        prop[1].cmd = DTV_FREQUENCY;
        prop[1].u.data = GetFrequency_S(tdata->table->set_freq);
        prop[2].cmd = DTV_STREAM_ID;
        prop[2].u.data = tdata->table->tsid;
        prop[3].cmd = DTV_TUNE;
        props.props = prop;
        props.num = 4;
        fprintf(stderr,"tuning to %d MHz\n",prop[1].u.data / 1000);
	}
    if(ioctl(tdata->fefd, FE_SET_PROPERTY, &props) == -1) {
        perror("ioctl FE_SET_PROPERTY");
        return 1;
    }
    return 0;
}

int
open_tuner(thread_data *tdata, int dev_num, boolean msg_view)
{
    char device[32];

    if(tdata->fefd == 0){
        sprintf(device, "/dev/dvb/adapter%d/frontend0", dev_num);
        tdata->fefd = open(device, O_RDWR);
        if(tdata->fefd < 0) {
            if(msg_view)
                fprintf(stderr, "cannot open frontend device\n");
            tdata->fefd = 0;
            return 1;
        }
        fprintf(stderr, "\rdevice = %s", device);
    }
	if(set_frequency(tdata, msg_view)){
		close(tdata->fefd);
		tdata->fefd = 0;
		return 1;
	}else
		return 0;
}

int
dvb_lock_check(thread_data *tdata)
{
	struct timespec ts;

	ts.tv_sec = 0;
#if 0
    struct dvb_frontend_event event;
    struct pollfd pfd[1];
    int rc, i;

    pfd[0].fd = tdata->fefd;
    pfd[0].events = POLLIN;
    event.status=0;
    i = 0;
    fprintf(stderr,"polling");
    do {
        fprintf(stderr,".");
        if (poll(pfd,1,5000)){
            if (pfd[0].revents & POLLIN){
                if((rc = ioctl(tdata->fefd, FE_GET_EVENT, &event)) < 0){
                    if (errno != EOVERFLOW) {
                        perror("ioctl FE_GET_EVENT");
                        fprintf(stderr,"status = %d\n", rc);
                        fprintf(stderr,"errno = %d\n", errno);
                        return -1;
                    }
                    else
                        fprintf(stderr,"\nOverflow error, trying again (status = %d, errno = %d)", rc, errno);
                }
            }
        }
    }while( (i++ < 5) && ((event.status & FE_TIMEDOUT)==0) && ((event.status & FE_HAS_LOCK)==0) );

    if((event.status & FE_HAS_LOCK)==0) {
        fprintf(stderr, "\nCannot lock to the signal on the given channel\n");
        return 1;
    }
	fprintf(stderr, "ok(0x%X)\n", event.status);
#else
	// from BonDriver_DVB.cpp
	// こっちの方が開始時のエラーデータが減る
	// LOCK後しばらくはエラーが交じるようなので応答性の悪いこちらの方が良いようだ
	fe_status_t status;
	int i;

	ts.tv_nsec = 250 * 1000 * 1000;	// 250ms
	for (i = 0; i < 12; i++)	// 250ms * 12で最大3秒待つ 3秒待ってもFE_HAS_LOCK後はstatus=0x13で変化無し
	{
        fprintf(stderr,".");
		if (ioctl(tdata->fefd, FE_READ_STATUS, &status) < 0)
		{
			fprintf(stderr, "dvb_lock_check() ioctl(FE_READ_STATUS) error\n");
	        return 1;
		}
		if (status & FE_HAS_LOCK)
		{
			fprintf(stderr, "ok(0x%X)\n", status);
	        goto SUCCESS_EXIT;
		}
		nanosleep(&ts, NULL);
	}
	fprintf(stderr, "dvb_lock_check() timeout");
    return 1;
SUCCESS_EXIT:;
#endif

// PT2地デジの開始時エラーパケット対策
// 環境依存ぽいが
#if 1
	if(tuner_type == EARTH_PT1){
		ts.tv_nsec = 200 * 1000 * 1000;	// 200ms
		nanosleep(&ts, NULL);
	}
#else
    // 取得C/Nが良好でも開始時はエラーまみれ…これ意味無し(;_;)
    // FE_READ_BER FE_READ_UNCORRECTED_BLOCKSはPT1ドライバー未実装
    int16_t rc;
    int32_t ublocks;

	ts.tv_nsec = 100 * 1000 * 1000;	// 100ms
	i = 0;
	do{
		if(ioctl(tdata->fefd, FE_READ_SNR, &rc) < 0){
			switch(errno){
			case EOPNOTSUPP:	// 95: Operation not supported on transport endpoint
				return 0;
			case EIO:			// 5: I/O error
				break;
			}
		}else
			if(rc >= 0){
				if(i >= 10)
					break;
			}
	if(ioctl(tdata->fefd, FE_READ_UNCORRECTED_BLOCKS, &ublocks)<0){
		if(i == 0)
			fprintf(stderr, "ERROR: FE_READ_UNCORRECTED_BLOCKS errno=%d(%s)\n", errno, strerror(errno));
	}else
		fprintf(stderr, "UNCORRECTED_BLOCKS: %d\n", ublocks);

		nanosleep(&ts, NULL);
	}while(++i < 50);		// 5sec
#endif
#if 0
	struct dtv_property prop[1];
	struct dtv_properties props;
	prop[0].cmd = DTV_CLEAR;
//	prop[0].u.data = 1;
	props.props = prop;
	props.num = 1;
	if (ioctl(tdata->fefd, FE_SET_PROPERTY, &props) < 0){
		fprintf(stderr, "dvb_lock_check() DTV_CLEAR failed\n");
    }
#endif
    return 0;
}

int
close_tuner(thread_data *tdata)
{
	struct dtv_property prop[1];
	struct dtv_properties props;
    int rv = 0;

    if(tdata->table->type == CHTYPE_SATELLITE && tdata->lnb >= 0 && tdata->lnb != SEC_VOLTAGE_OFF) {
		prop[0].cmd = DTV_VOLTAGE;
		prop[0].u.data = SEC_VOLTAGE_OFF;
		props.props = prop;
		props.num = 1;

		if (ioctl(tdata->fefd, FE_SET_PROPERTY, &props) < 0){
			fprintf(stderr, "LNB OFF failed\n");
            rv = 1;
        }
    }

	if(tdata->table == &isdb_t_conv_set)
		tdata->table = NULL;
    if(tdata->tfd != -1){
	    close(tdata->tfd);
	    tdata->tfd = -1;
	}
    if(tdata->dmxfd > 0){
      close(tdata->dmxfd);
      tdata->dmxfd = 0;
    }
    if(tdata->fefd > 0){
      close(tdata->fefd);
      tdata->fefd = 0;
    }

    return rv;
}

void
stream_start(thread_data *tdata)
{
	if(ioctl(tdata->dmxfd, DMX_START) != 0)
		fprintf(stderr, "stream_stop() failed\n");
}

void
stream_stop(thread_data *tdata)
{
	if(ioctl(tdata->dmxfd, DMX_STOP) != 0)
		fprintf(stderr, "stream_stop() failed\n");
}

int
lnb_control(int dev_num, int lnb_vol)
{
    struct dtv_property prop[1];
    struct dtv_properties props;
    struct dvb_frontend_info fe_info;
    char device[32];
    int fefd;

    sprintf(device, "/dev/dvb/adapter%d/frontend0", dev_num);
    fefd = open(device, O_RDWR);
    if(fefd < 0) {
        fprintf(stderr, "cannot open \"%s\"\n", device);
        return 1;
    }
    fprintf(stderr, "\rdevice = %s", device);
    if( (ioctl(fefd,FE_GET_INFO, &fe_info) < 0)){
        fprintf(stderr, "FE_GET_INFO failed\n");
		close(fefd);
        return 1;
    }
    fprintf(stderr,"\nUsing DVB device \"%s\"\n",fe_info.name);
    if(fe_info.type != FE_QPSK){
        fprintf(stderr, "tuner is not BS/CS110(FE_QPSK)\n");
		close(fefd);
        return 1;
    }
	prop[0].cmd = DTV_VOLTAGE;
	prop[0].u.data = lnb_vol;
	props.props = prop;
	props.num = 1;
	if (ioctl(fefd, FE_SET_PROPERTY, &props) < 0){
		fprintf(stderr, "LNB control failed");
		close(fefd);
        return 1;
    }
    close(fefd);
    return 0;
}

void
calc_cn(int fd, int type, boolean use_bell)
{
	double  CNR;
	int bell = 0;

	struct dtv_property prop[2];
	struct dtv_properties props;

	prop[0].cmd = DTV_STAT_CNR;
	prop[1].cmd = DTV_STAT_SIGNAL_STRENGTH;
	props.props = prop;
	props.num = 2;
	if (ioctl(fd, FE_GET_PROPERTY, &props) < 0){
		fprintf(stderr, "ERROR: calc_cn() ioctl(FE_GET_PROPERTY) errno=%d(%s)\n", errno, strerror(errno));
		return;
	}

	if (prop[0].u.st.stat[0].scale == FE_SCALE_DECIBEL){
		CNR = prop[0].u.st.stat[0].svalue / 1000.0;
		if(CNR >= 30.0)
			bell = 3;
		else if(CNR >= 15.0 && CNR < 30.0)
			bell = 2;
		else if(CNR < 15.0)
			bell = 1;
		fprintf(stderr, "C/N = %.4fdB\n", CNR);
		if(use_bell) {
			do_bell(bell);
		}
	} else if (prop[1].u.st.stat[0].scale == FE_SCALE_RELATIVE){
		fprintf(stderr, "C/N = %.4f%%\n", prop[0].u.st.stat[0].uvalue / 65535.0 * 100);
	}
	if (prop[1].u.st.stat[0].scale == FE_SCALE_DECIBEL){
		fprintf(stderr, "RF level = %.4fdBm\n", prop[1].u.st.stat[0].svalue / 1000.0);
	} else if (prop[1].u.st.stat[1].scale == FE_SCALE_RELATIVE){
		fprintf(stderr, "RF level = %.4f%%\n", prop[1].u.st.stat[0].uvalue / 65535.0 * 100);
	}
	return;
}

void
show_channels(void)
{
	FILE *f;
	char *home;
	char buf[255], filename[255];

	fprintf(stderr, "Available Channels:\n");

	home = getenv("HOME");
	sprintf(filename, "%s/.recpt1-channels", home);
	f = fopen(filename, "r");
	if(f) {
		while(fgets(buf, 255, f))
			fprintf(stderr, "%s", buf);
		fclose(f);
	}
	else
		fprintf(stderr, "13-62: Terrestrial Channels\n");

	fprintf(stderr, "101ch: NHK BS1\n");
	fprintf(stderr, "103ch: NHK BS Premium\n");
	fprintf(stderr, "141ch: BS Nittele\n");
	fprintf(stderr, "151ch: BS Asahi\n");
	fprintf(stderr, "161ch: BS-TBS\n");
	fprintf(stderr, "171ch: BS Japan\n");
	fprintf(stderr, "181ch: BS Fuji\n");
	fprintf(stderr, "191ch: WOWOW Prime\n");
	fprintf(stderr, "192ch: WOWOW Live\n");
	fprintf(stderr, "193ch: WOWOW Cinema\n");
	fprintf(stderr, "200ch: Star Channel1\n");
	fprintf(stderr, "201ch: Star Channel2\n");
	fprintf(stderr, "202ch: Star Channel3\n");
	fprintf(stderr, "211ch: BS11 Digital\n");
	fprintf(stderr, "222ch: TwellV\n");
	fprintf(stderr, "231ch: Housou Daigaku 1\n");
	fprintf(stderr, "232ch: Housou Daigaku 2\n");
	fprintf(stderr, "233ch: Housou Daigaku 3\n");
	fprintf(stderr, "234ch: Green Channel\n");
	fprintf(stderr, "236ch: BS Animax\n");
	fprintf(stderr, "238ch: FOX bs238\n");
	fprintf(stderr, "241ch: BS SkyPer!\n");
	fprintf(stderr, "242ch: J SPORTS 1\n");
	fprintf(stderr, "243ch: J SPORTS 2\n");
	fprintf(stderr, "244ch: J SPORTS 3\n");
	fprintf(stderr, "245ch: J SPORTS 4\n");
	fprintf(stderr, "251ch: BS Tsuri Vision\n");
	fprintf(stderr, "252ch: IMAGICA BS\n");
	fprintf(stderr, "255ch: Nihon Eiga Senmon Channel\n");
	fprintf(stderr, "256ch: Disney Channel\n");
	fprintf(stderr, "258ch: Dlife\n");
	fprintf(stderr, "C13-C63: CATV Channels\n");
	fprintf(stderr, "CS2-CS24: CS Channels\n");
	fprintf(stderr, "BS1_0-BS23_1: BS Channels(Transport)\n");
	fprintf(stderr, "0x4000-0x7FFF: BS/CS Channels(TSID)\n");
}


int
parse_time(char *rectimestr, int *recsec)
{
    /* indefinite */
    if(!strcmp("-", rectimestr)) {
        *recsec = -1;
        return 0;
    }
    /* colon */
    else if(strchr(rectimestr, ':')) {
        int n1, n2, n3;
        if(sscanf(rectimestr, "%d:%d:%d", &n1, &n2, &n3) == 3)
            *recsec = n1 * 3600 + n2 * 60 + n3;
        else if(sscanf(rectimestr, "%d:%d", &n1, &n2) == 2)
            *recsec = n1 * 3600 + n2 * 60;
        else
            return 1; /* unsuccessful */

        return 0;
    }
    /* HMS */
    else {
        char *tmpstr;
        char *p1, *p2;
        int  flag;

        if( *rectimestr == '-' ){
	        rectimestr++;
	        flag = 1;
	    }else
	        flag = 0;
        tmpstr = strdup(rectimestr);
        p1 = tmpstr;
        while(*p1 && !isdigit(*p1))
            p1++;

        /* hour */
        if((p2 = strchr(p1, 'H')) || (p2 = strchr(p1, 'h'))) {
            *p2 = '\0';
            *recsec += atoi(p1) * 3600;
            p1 = p2 + 1;
            while(*p1 && !isdigit(*p1))
                p1++;
        }

        /* minute */
        if((p2 = strchr(p1, 'M')) || (p2 = strchr(p1, 'm'))) {
            *p2 = '\0';
            *recsec += atoi(p1) * 60;
            p1 = p2 + 1;
            while(*p1 && !isdigit(*p1))
                p1++;
        }

        /* second */
        *recsec += atoi(p1);
        if( flag )
        	*recsec *= -1;

        free(tmpstr);

        return 0;
    } /* else */

    return 1; /* unsuccessful */
}

void
do_bell(int bell)
{
    int i;
    for(i=0; i < bell; i++) {
        fprintf(stderr, "\a");
        usleep(400000);
    }
}

int
selects(const struct dirent *dir)
{
	if(strncmp(dir->d_name, "adapter", 7)){
		return 0;
	}
	return 1;
}

void
close_dir(struct dirent **namelist, int namecnts)
{
	int i;

	for(i = 0; i < namecnts; i++)
		free(namelist[i]);
	free(namelist);
}

/* from checksignal.c */
int
tune(char *channel, thread_data *tdata, int dev_num)
{
    struct dmx_pes_filter_params filter;
    char device[32];

    /* get channel */
    tdata->table = searchrecoff(channel);
    if(tdata->table == NULL) {
        fprintf(stderr, "Invalid Channel: %s\n", channel);
        return 1;
    }

    /* open tuner */
    /* case 1: specified tuner device */
    if(dev_num >= 0) {
		if(open_tuner(tdata, dev_num, TRUE) != 0)
			return 1;
		while(dvb_lock_check(tdata) != 0){
            if(tdata->tune_persistent) {
                if(f_exit) {
                    close_tuner(tdata);
                    return 1;
                }
                fprintf(stderr, "No signal. Still trying: /dev/dvb/adapter%d\n", dev_num);
            }
            else {
                close_tuner(tdata);
                fprintf(stderr, "Cannot tune to the specified channel: /dev/dvb/adapter%d\n", dev_num);
                return 1;
            }
		}
	}else{
        /* case 2: loop around available devices */
#if FULLAUTO_SEARCH
		struct dirent **namelist;
		int lp;
		int num_devs = scandir("/dev/dvb", &namelist, selects, alphasort);
		boolean tuned = FALSE;

		if(num_devs == -1) {
			perror("scandir");
			return 1;
		}
		for (lp = 0; lp < num_devs; lp++) {
            int count = 0;

			sscanf(namelist[lp]->d_name, "adapter%d", &dev_num);
#else
        int *tuner;
        int num_devs,lp;

        if(tdata->table->type == CHTYPE_SATELLITE) {
            tuner = bsdev;
            num_devs = NUM_BSDEV;
        }
        else {
            tuner = isdb_t_dev;
            num_devs = NUM_ISDB_T_DEV;
        }

        for(lp = 0; lp < num_devs; lp++) {
            int count = 0;

            dev_num = tuner[lp];
#endif
			if(open_tuner(tdata, dev_num, FALSE) == 0){
                /* tune to specified channel */
                if(tdata->tune_persistent) {
					while(dvb_lock_check(tdata) != 0 &&
                          count < MAX_RETRY) {
                        if(f_exit) {
                            close_tuner(tdata);
#if FULLAUTO_SEARCH
							close_dir(namelist, num_devs);
#endif
                            return 1;
                        }
                        fprintf(stderr, "No signal. Still trying: /dev/dvb/adapter%d\n", dev_num);
                        count++;
                    }

                    if(count >= MAX_RETRY) {
                        close_tuner(tdata);
                        count = 0;
                        continue;
                    }
                } /* tune_persistent */
                else {
					if(dvb_lock_check(tdata) != 0){
                        close_tuner(tdata);
                        tdata->tfd = -1;
                        continue;
                    }
                }

                tuned = TRUE;
                if(tdata->tune_persistent)
                    fprintf(stderr, "device = /dev/dvb/adapter%d\n", dev_num);
                break; /* found suitable tuner */
            }
#if FULLAUTO_SEARCH
        }
		close_dir(namelist, num_devs);
#else
        }
#endif
        /* all tuners cannot be used */
        if(tuned == FALSE) {
            fprintf(stderr, "Cannot tune to the specified channel\n");
            return 1;
        }
	}

    if(tdata->dmxfd == 0){
        sprintf(device, "/dev/dvb/adapter%d/demux0", dev_num);
        if((tdata->dmxfd = open(device,O_RDWR)) < 0){
            tdata->dmxfd = 0;
            fprintf(stderr, "cannot open demux device\n");
            return 1;
        }
    }
#if 0
//	if(ioctl(tdata->dmxfd, DMX_SET_BUFFER_SIZE, DMX_BUFFER_SIZE) != 0)
//		perror("ioctl DMX_SET_BUFFER_SIZE");
	struct dtv_property prop[1];
	struct dtv_properties props;
	prop[0].cmd = DTV_CLEAR;
//	prop[0].u.data = 1;
	props.props = prop;
	props.num = 1;
	if (ioctl(tdata->fefd, FE_SET_PROPERTY, &props) < 0){
		fprintf(stderr, "DTV_CLEAR failed:(%d)%s\n", errno, strerror(errno));
    }
#endif

    filter.pid = 0x2000;
    filter.input = DMX_IN_FRONTEND;
    filter.output = DMX_OUT_TS_TAP;
//  filter.pes_type = DMX_PES_OTHER;
    filter.pes_type = DMX_PES_VIDEO;
    filter.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;
    if (ioctl(tdata->dmxfd, DMX_SET_PES_FILTER, &filter) == -1) {
        fprintf(stderr,"FILTER %i: ", filter.pid);
        perror("ioctl DMX_SET_PES_FILTER");
        close(tdata->dmxfd);
        tdata->dmxfd = 0;
        return 1;
    }

    if(tdata->tfd < 0){
        sprintf(device, "/dev/dvb/adapter%d/dvr0", dev_num);
        if((tdata->tfd = open(device,O_RDONLY)) < 0){
//      if((tdata->tfd = open(device,O_RDONLY|O_NONBLOCK)) < 0){
            fprintf(stderr, "cannot open dvr device\n");
            close(tdata->dmxfd);
            tdata->dmxfd = 0;
            return 1;
        }
    }

    if(!tdata->tune_persistent) {
        /* show signal strength */
        calc_cn(tdata->fefd, tdata->table->type, FALSE);
    }

    return 0; /* success */
}
