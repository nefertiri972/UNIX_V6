#
/*
 */

#include "../param.h"
#include "../systm.h"
#include "../user.h"
#include "../proc.h"
#include "../buf.h"
#include "../reg.h"
#include "../inode.h"

/*
 * exec system call.
 * Because of the fact that an I/O buffer is used
 * to store the caller's arguments during exec,
 * and more buffers are needed to read in the text file,
 * deadly embraces waiting for free buffers are possible.
 * Therefore the number of processes simultaneously
 * running in exec has to be limited to NEXEC.
 */
#define EXPRI	-1

exec()
{
	int ap, na, nc, *bp;
	int ts, ds, sep;
	register c, *ip;
	register char *cp;
	extern uchar;

	/*
	 * pick up file names
	 * and check various modes
	 * for execute permission
	 */

	ip = namei(&uchar, 0);							// ip에 inode[] 엔트리를 가져옴  
	if(ip == NULL)
		return;
	while(execnt >= NEXEC)
		sleep(&execnt, EXPRI);
	execnt++;
	bp = getblk(NODEV);								// 사용중이지 않은 블럭 디바이스 버퍼 할당
	if(access(ip, IEXEC) || (ip->i_mode&IFMT)!=0)	// inode의 실행파일의 실행 권한을 검사, 스페셜 파일인지 검사
		goto bad;

	/*
	 * pack up arguments into
	 * allocated disk buffer
	 */

	cp = bp->b_addr;	// 버퍼 어드레스 저장
	na = 0;				// 매개변수의 수
	nc = 0;				// 매개변수의 총 Byte 수
	while(ap = fuword(u.u_arg[1])) {	
		na++;
		if(ap == -1)
			goto bad;
		u.u_arg[1] =+ 2;
		for(;;) {
			c = fubyte(ap++);
			if(c == -1)
				goto bad;
			*cp++ = c;
			nc++;
			if(nc > 510) {
				u.u_error = E2BIG;
				goto bad;
			}
			if(c == 0)
				break;
		}
	}
	if((nc&1) != 0) {	// nc가 홀수일 경우 워드단위로 처리하기 위해 한 바이트를 더함
		*cp++ = 0;
		nc++;
	}

	/*
	 * read in first 8 bytes
	 * of file for segment
	 * sizes:
	 * w0 = 407/410/411 (410 implies RO text) (411 implies sep ID)
	 * w1 = text size
	 * w2 = data size
	 * w3 = bss size
	 */

	u.u_base = &u.u_arg[0];
	u.u_count = 8;
	u.u_offset[1] = 0;
	u.u_offset[0] = 0;
	u.u_segflg = 1;
	readi(ip);
	u.u_segflg = 0;
	if(u.u_error)
		goto bad;
	sep = 0;
	if(u.u_arg[0] == 0407) {
		u.u_arg[2] =+ u.u_arg[1];
		u.u_arg[1] = 0;
	} else
	if(u.u_arg[0] == 0411)
		sep++; else
	if(u.u_arg[0] != 0410) {
		u.u_error = ENOEXEC;
		goto bad;
	}
	if(u.u_arg[1]!=0 && (ip->i_flag&ITEXT)==0 && ip->i_count!=1) {
		u.u_error = ETXTBSY;
		goto bad;
	}

	/*
	 * find text and data sizes
	 * try them out for possible
	 * exceed of max sizes
	 */

	ts = ((u.u_arg[1]+63)>>6) & 01777;					// +63은 64보다 작을 경우
	ds = ((u.u_arg[2]+u.u_arg[3]+63)>>6) & 01777;
	if(estabur(ts, ds, SSIZE, sep))
		goto bad;

	/*
	 * allocate and clear core
	 * at this point, committed
	 * to the new image
	 */

	u.u_prof[3] = 0;
	xfree();
	expand(USIZE);
	xalloc(ip);
	c = USIZE+ds+SSIZE;
	expand(c);
	while(--c >= USIZE)
		clearseg(u.u_procp->p_addr+c);

	/*
	 * read in data segment
	 */

	estabur(0, ds, 0, 0);
	u.u_base = 0;
	u.u_offset[1] = 020+u.u_arg[1];
	u.u_count = u.u_arg[2];
	readi(ip);

	/*
	 * initialize stack segment
	 */

	u.u_tsize = ts;
	u.u_dsize = ds;
	u.u_ssize = SSIZE;
	u.u_sep = sep;
	estabur(u.u_tsize, u.u_dsize, u.u_ssize, u.u_sep);
	cp = bp->b_addr;
	ap = -1 - na*2 - 4;
	u.u_ar0[R6] = ap;
	suword(ap, na);
	c = -nc;
	while(na--) {
		suword(ap=+2, c);
		do
			subyte(c++, *cp);
		while(*cp++);
	}
	suword(ap+2, -1);

	/*
	 * set SUID/SGID protections, if no tracing
	 */

	if ((u.u_procp->p_flag&STRC)==0) {
		if(ip->i_mode&ISUID)
			if(u.u_uid != 0) {
				u.u_uid = ip->i_uid;
				u.u_procp->p_uid = ip->i_uid;
			}
		if(ip->i_mode&ISGID)
			u.u_gid = ip->i_gid;
	}

	/*
	 * clear sigs, regs and return
	 */

	c = ip;
	for(ip = &u.u_signal[0]; ip < &u.u_signal[NSIG]; ip++)
		if((*ip & 1) == 0)
			*ip = 0;
	for(cp = &regloc[0]; cp < &regloc[6];)
		u.u_ar0[*cp++] = 0;
	u.u_ar0[R7] = 0;
	for(ip = &u.u_fsav[0]; ip < &u.u_fsav[25];)
		*ip++ = 0;
	ip = c;

bad:
	iput(ip);
	brelse(bp);
	if(execnt >= NEXEC)
		wakeup(&execnt);
	execnt--;
}

/*
 * exit system call:
 * pass back caller's r0
 */
rexit()
{

	u.u_arg[0] = u.u_ar0[R0] << 8;
	exit();
}

/*
 * Release resources.
 * Save u. area for parent to look at.
 * Enter zombie state.
 * Wake up parent and init processes,
 * and dispose of children.
 */
exit()
{
	register int *q, a;
	register struct proc *p;

	u.u_procp->p_flag =& ~STRC;								//�듃�젅�씠�뒪 �뵆�젅洹� 臾댄슚�솕(>>)
	for(q = &u.u_signal[0]; q < &u.u_signal[NSIG];)			//�떆洹몃꼸 臾댁떆�븯湲� �쐞�빐 u.usignal 紐⑤몢 1(>>) //#define NSIG 20
		*q++ = 1;
	for(q = &u.u_ofile[0]; q < &u.u_ofile[NOFILE]; q++)		//�봽濡쒖꽭�뒪媛� �삤�뵂�븳 �뙆�씪 紐⑤몢 close //#define NOFILE 15
		if(a = *q) {
			*q = NULL;
			closef(a);
		}
	iput(u.u_cdir);				//�쁽�옱 �뵒�젆�넗由� 李몄“ 移댁슫�꽣 媛먯냼(>>)
	xfree();					//�뀓�뒪�듃 �꽭洹몃㉫�듃 �빐�젣
	a = malloc(swapmap, 1);		//�뒪����봽 �쁺�뿭 �솗蹂� swapmap(�뒪����븨 怨듦컙)�쓽 二쇱냼瑜� 留ㅺ컻蹂��닔濡�
	if(a == NULL)
		panic("out of swap");
	p = getblk(swapdev, a);		//釉붾줉 �뵒諛붿씠�뒪�쓽 踰꾪띁瑜� �뼸�뒗�떎(�쐞�쓽 malloc 怨� �떎瑜몄젏(??))
	bcopy(&u, p->b_addr, 256);	//釉붾줉 �뵒諛붿씠�뒪�쓽 踰꾪띁�뿉 512諛붿씠�듃(�뜲�씠�꽣 �꽭洹몃㉫�듃) ����옣
	bwrite(p);					//釉붾줉 �뵒諛붿씠�뒪 �뒪����봽 �쁺�뿭�뿉 ����옣
	q = u.u_procp;
	mfree(coremap, q->p_size, q->p_addr);
	q->p_addr = a;
	q->p_stat = SZOMB;			//�뒪����봽 �맂 �젙蹂대�� ����옣(�옱�떎�뻾 �릺吏� �븡�쓬)

loop:
	for(p = &proc[0]; p < &proc[NPROC]; p++)
	if(q->p_ppid == p->p_pid) {		//遺�紐� �봽濡쒖꽭�뒪 李얠쓬
		wakeup(&proc[1]);			//Init �봽濡쒖꽭�뒪 源⑥��
		wakeup(p);					//遺�紐� �봽濡쒖꽭�뒪 源⑥��
		for(p = &proc[0]; p < &proc[NPROC]; p++)
		if(q->p_pid == p->p_ppid) {	//�옄�떇 �봽濡쒖꽭�뒪 李얠쓬
			p->p_ppid  = 1;			//Init�쓽 �옄�떇�쑝濡� 留뚮벀
			if (p->p_stat == SSTOP)
				setrun(p);			//�듃�젅�씠�뒪 湲곕떎由щ뒗 �긽�깭硫� �떎�뻾媛��뒗 �긽�깭濡� 蹂�寃�
		}
		swtch();					//�떎�뻾 �봽濡쒖꽭�뒪 諛붽퓞
		/* no return */
	}
	q->p_ppid = 1;					//遺�紐� �봽濡쒖꽭�뒪瑜� Init�쑝濡� 留뚮벀(�뼱�뼡 �삤瑜섎줈 �씤�빐�꽌 遺�紐� �봽濡쒖꽭�뒪媛� �뾾�쓣 �븣)
	goto loop;
}

/*
 * Wait system call.
 * Search for a terminated (zombie) child,
 * finally lay it to rest, and collect its status.
 * Look also for stopped (traced) children,
 * and pass back status from them.
 */
wait()
{
	register f, *bp;
	register struct proc *p;

	f = 0;
loop:
	for(p = &proc[0]; p < &proc[NPROC]; p++)
	if(p->p_ppid == u.u_procp->p_pid) {
		f++;										//�옄�떇 �봽濡쒖꽭�뒪 李얘퀬, �닔 利앷��
		if(p->p_stat == SZOMB) {
			u.u_ar0[R0] = p->p_pid;					//�옄�떇�쓽 pid瑜� R0�뿉 ����옣
			bp = bread(swapdev, f=p->p_addr);
			mfree(swapmap, 1, f);
			p->p_stat = NULL;
			p->p_pid = 0;
			p->p_ppid = 0;
			p->p_sig = 0;
			p->p_ttyp = 0;
			p->p_flag = 0;
			p = bp->b_addr;
			u.u_cstime[0] =+ p->u_cstime[0];
			dpadd(u.u_cstime, p->u_cstime[1]);
			dpadd(u.u_cstime, p->u_stime);
			u.u_cutime[0] =+ p->u_cutime[0];
			dpadd(u.u_cutime, p->u_cutime[1]);
			dpadd(u.u_cutime, p->u_utime);
			u.u_ar0[R1] = p->u_arg[0];				//�궗�슜�옄 �봽濡쒖꽭�뒪 R1�뿉 u_arg[0] ����옣, �옄�떇�봽濡쒖꽭�뒪媛� 醫낅즺 �긽�깭�씤 寃껋쓣 �븣 �닔 �엳�떎.
			brelse(bp);
			return;
		}
		if(p->p_stat == SSTOP) {					//�듃�젅�씠�뒪 泥섎━(>>)
			if((p->p_flag&SWTED) == 0) {
				p->p_flag =| SWTED;
				u.u_ar0[R0] = p->p_pid;
				u.u_ar0[R1] = (p->p_sig<<8) | 0177;
				return;
			}
			p->p_flag =& ~(STRC|SWTED);
			setrun(p);
		}
	}
	if(f) {
		sleep(u.u_procp, PWAIT);		//�옄�떇 �봽濡쒖꽭�뒪�뿉 醫�鍮꾩긽�깭媛� �뾾�쑝硫� sleep �긽�깭媛� �릺�뼱 �옄�떇 �봽濡쒖꽭�뒪 �걹�궇 �븣 源뚯�� ���湲�
		goto loop;
	}
	u.u_error = ECHILD;
}

/*
 * fork system call.
 */
fork()
{
	register struct proc *p1, *p2;

	p1 = u.u_procp;
	for(p2 = &proc[0]; p2 < &proc[NPROC]; p2++)
		if(p2->p_stat == NULL)
			goto found;
	u.u_error = EAGAIN;
	goto out;

found:
	if(newproc()) {
		u.u_ar0[R0] = p1->p_pid;
		u.u_cstime[0] = 0;
		u.u_cstime[1] = 0;
		u.u_stime = 0;
		u.u_cutime[0] = 0;
		u.u_cutime[1] = 0;
		u.u_utime = 0;
		return;
	}
	u.u_ar0[R0] = p2->p_pid;

out:
	u.u_ar0[R7] =+ 2;
}

/*
 * break system call.
 *  -- bad planning: "break" is a dirty word in C.
 */
sbreak()
{
	register a, n, d;
	int i;

	/*
	 * set n to new data size
	 * set d to new-old
	 * set n to new total size
	 */

	n = (((u.u_arg[0]+63)>>6) & 01777);
	if(!u.u_sep)
		n =- nseg(u.u_tsize) * 128;
	if(n < 0)
		n = 0;
	d = n - u.u_dsize;
	n =+ USIZE+u.u_ssize;
	if(estabur(u.u_tsize, u.u_dsize+d, u.u_ssize, u.u_sep))
		return;
	u.u_dsize =+ d;
	if(d > 0)
		goto bigger;
	a = u.u_procp->p_addr + n - u.u_ssize;
	i = n;
	n = u.u_ssize;
	while(n--) {
		copyseg(a-d, a);
		a++;
	}
	expand(i);
	return;

bigger:
	expand(n);
	a = u.u_procp->p_addr + n;
	n = u.u_ssize;
	while(n--) {
		a--;
		copyseg(a-d, a);
	}
	while(d--)
		clearseg(--a);
}
