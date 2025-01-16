#include "BonDriverProxy.h"

#ifdef HAVE_LIBARIBB25
#include <getopt.h>
static BOOL g_b25_enable = FALSE;
#endif

namespace BonDriverProxy {

#define STRICT_LOCK

static BOOL g_bStop;	// 初期値FALSE
static void Handler(int sig)
{
	g_bStop = TRUE;
}

static void CleanUp()
{
	if (g_DisableUnloadBonDriver)
	{
		while (!g_LoadedDriverList.empty())
		{
			stLoadedDriver *pLd = g_LoadedDriverList.front();
			g_LoadedDriverList.pop_front();
			::dlclose(pLd->hModule);
			::fprintf(stderr, "[%s] unloaded\n", pLd->strBonDriver);
			delete pLd;
		}
	}
}

static int Init(int ac, char *av[])
{
#ifdef HAVE_LIBARIBB25
	static const struct option options[] = {
		{"b25",         no_argument, &g_b25_enable, TRUE},
		{"strip",       no_argument, &B25Decoder::strip, 1},
		{"emm",         no_argument, &B25Decoder::emm_proc, 1},
		{"round", required_argument, NULL, 'r'},
		{0}
	};

	int opt;

	while ((opt = getopt_long(ac, av, "", options, NULL)) != -1)
	{
		switch (opt)
		{
		case 0:		// long options
			break;
		case 'r':	// multi2 round
			B25Decoder::multi2_round = strtoul(optarg, NULL, 10);
			break;
		default:
			return -1;
		}
	}

	if (optind > 1)
	{
		int i = optind, j = 1;
		while (i < ac)
			av[j++] = av[i++];
		ac -= optind - 1;
	}
#endif
	if (ac < 3)
		return -1;
	::strncpy(g_Host, av[1], sizeof(g_Host) - 1);
	g_Host[sizeof(g_Host) - 1] = '\0';
	::strncpy(g_Port, av[2], sizeof(g_Port) - 1);
	g_Port[sizeof(g_Port) - 1] = '\0';
	if (ac > 3)
	{
		g_PacketFifoSize = ::atoi(av[3]);
		if (ac > 4)
			g_TsPacketBufSize = ::atoi(av[4]);
	}
	return 0;
}

cProxyServer::cProxyServer() : m_Error(m_c, m_m), m_fifoSend(m_c, m_m), m_fifoRecv(m_c, m_m)
{
	m_s = INVALID_SOCKET;
	m_hModule = NULL;
	m_pIBon = m_pIBon2 = m_pIBon3 = NULL;
	m_strBonDriver[0] = '\0';
	m_bTunerOpen = FALSE;
	m_bChannelLock = 0;
	m_hTsRead = 0;
	m_pTsReaderArg = NULL;

	pthread_mutexattr_t attr;
	::pthread_mutexattr_init(&attr);
	::pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	::pthread_mutex_init(&m_m, &attr);
	::pthread_cond_init(&m_c, NULL);
}

cProxyServer::~cProxyServer()
{
	LOCK(g_Lock);
	BOOL bRelease = TRUE;
	std::list<cProxyServer *>::iterator it = g_InstanceList.begin();
	while (it != g_InstanceList.end())
	{
		if (*it == this)
			g_InstanceList.erase(it++);
		else
		{
			if ((m_hModule != NULL) && (m_hModule == (*it)->m_hModule))
				bRelease = FALSE;
			++it;
		}
	}
	if (bRelease)
	{
		if (m_hTsRead)
		{
			m_pTsReaderArg->StopTsRead = TRUE;
			::pthread_join(m_hTsRead, NULL);
			delete m_pTsReaderArg;
		}
		if (m_pIBon)
			m_pIBon->Release();
		if (m_hModule)
		{
			if (!g_DisableUnloadBonDriver)
			{
				::dlclose(m_hModule);
#ifdef DEBUG
				::fprintf(stderr, "[%s] unloaded\n", m_strBonDriver);
#endif
			}
		}
	}
	else
	{
		if (m_hTsRead)
		{
			m_pTsReaderArg->TsLock.Enter();
			it = m_pTsReaderArg->TsReceiversList.begin();
			while (it != m_pTsReaderArg->TsReceiversList.end())
			{
				if (*it == this)
				{
					m_pTsReaderArg->TsReceiversList.erase(it);
					break;
				}
				++it;
			}
			m_pTsReaderArg->TsLock.Leave();

			// このインスタンスはチャンネル排他権を持っているか？
			if (m_bChannelLock == 0xff)
			{
				// 持っていた場合は、排他権取得待ちのインスタンスは存在しているか？
				if (m_pTsReaderArg->WaitExclusivePrivList.size() > 0)
				{
					// 存在する場合は、リスト先頭のインスタンスに排他権を引き継ぎ、リストから削除
					cProxyServer *p = m_pTsReaderArg->WaitExclusivePrivList.front();
					m_pTsReaderArg->WaitExclusivePrivList.pop_front();
					p->m_bChannelLock = 0xff;
				}
			}
			else
			{
				// 持っていない場合は、排他権取得待ちリストに自身が含まれているかもしれないので削除
				m_pTsReaderArg->WaitExclusivePrivList.remove(this);
			}

			// 可能性は低いがゼロではない…
			if (m_pTsReaderArg->TsReceiversList.empty())
			{
				m_pTsReaderArg->StopTsRead = TRUE;
				::pthread_join(m_hTsRead, NULL);
				delete m_pTsReaderArg;
			}
		}
	}

	::pthread_cond_destroy(&m_c);
	::pthread_mutex_destroy(&m_m);

	if (m_s != INVALID_SOCKET) {
		::fprintf(stderr,"sock%d: disconnect\n", m_s);
		::close(m_s);
	}
}

void *cProxyServer::Reception(LPVOID pv)
{
	cProxyServer *pProxy = static_cast<cProxyServer *>(pv);
	pProxy->Process();
	delete pProxy;
	return NULL;
}

DWORD cProxyServer::Process()
{
	pthread_t hThread[2];
	if (::pthread_create(&hThread[0], NULL, cProxyServer::Sender, this))
		return 1;

	if (::pthread_create(&hThread[1], NULL, cProxyServer::Receiver, this))
	{
		m_Error.Set();
		::pthread_join(hThread[0], NULL);
		return 2;
	}

	cEvent *h[2] = { &m_Error, m_fifoRecv.GetEventHandle() };
	for (;;)
	{
		DWORD dwRet = cEvent::MultipleWait(2, h);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
#ifdef STRICT_LOCK
			LOCK(g_Lock);
#endif
			cPacketHolder *pPh = NULL;
			m_fifoRecv.Pop(&pPh);
#ifdef DEBUG
			{
				const char *CommandName[]={
					"eSelectBonDriver",
					"eCreateBonDriver",
					"eOpenTuner",
					"eCloseTuner",
					"eSetChannel1",
					"eGetSignalLevel",
					"eWaitTsStream",
					"eGetReadyCount",
					"eGetTsStream",
					"ePurgeTsStream",
					"eRelease",

					"eGetTunerName",
					"eIsTunerOpening",
					"eEnumTuningSpace",
					"eEnumChannelName",
					"eSetChannel2",
					"eGetCurSpace",
					"eGetCurChannel",

					"eGetTotalDeviceNum",
					"eGetActiveDeviceNum",
					"eSetLnbPower",

					"eGetClientInfo",
				};
				if (pPh->GetCommand() <= eGetClientInfo)
				{
					::fprintf(stderr, "Recieve Command : [%s]\n", CommandName[pPh->GetCommand()]);
				}
				else
				{
					::fprintf(stderr, "Illegal Command : [%d]\n", (int)(pPh->GetCommand()));
				}
			}
#endif
			switch (pPh->GetCommand())
			{
			case eSelectBonDriver:
			{
				::fprintf(stderr, "sock%d: SelectBonDriver: [%s]\n", m_s, (LPCSTR)(pPh->m_pPacket->payload));

				if (pPh->GetBodyLength() <= sizeof(char))
					makePacket(eSelectBonDriver, FALSE);
				else
				{
					LPCSTR p = (LPCSTR)(pPh->m_pPacket->payload);
					if (::strlen(p) > (sizeof(m_strBonDriver) - 1))
						makePacket(eSelectBonDriver, FALSE);
					else
					{
						BOOL bFind = FALSE;
#ifndef STRICT_LOCK
						LOCK(g_Lock);
#endif
						for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
						{
							if (::strcmp(p, (*it)->m_strBonDriver) == 0)
							{
								bFind = TRUE;
								m_hModule = (*it)->m_hModule;
								::strcpy(m_strBonDriver, (*it)->m_strBonDriver);
								m_pIBon = (*it)->m_pIBon;	// (*it)->m_pIBonがNULLの可能性はゼロではない
								m_pIBon2 = (*it)->m_pIBon2;
								m_pIBon3 = (*it)->m_pIBon3;
								break;
							}
						}
						BOOL bSuccess;
						if (!bFind)
						{
							bSuccess = SelectBonDriver(p);
							if (bSuccess)
							{
								g_InstanceList.push_back(this);
								::strcpy(m_strBonDriver, p);
							}
						}
						else
						{
							g_InstanceList.push_back(this);
							bSuccess = TRUE;
						}
						makePacket(eSelectBonDriver, bSuccess);
					}
				}
				break;
			}

			case eCreateBonDriver:
			{
				if (m_pIBon == NULL)
				{
					BOOL bFind = FALSE;
					{
#ifndef STRICT_LOCK
						LOCK(g_Lock);
#endif
						for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
						{
							if (*it == this)
								continue;
							if (m_hModule == (*it)->m_hModule)
							{
								if ((*it)->m_pIBon != NULL)
								{
									bFind = TRUE;	// ここに来るのはかなりのレアケースのハズ
									m_pIBon = (*it)->m_pIBon;
									m_pIBon2 = (*it)->m_pIBon2;
									m_pIBon3 = (*it)->m_pIBon3;
									break;
								}
								// ここに来るのは上より更にレアケース、あるいはクライアントが
								// BonDriver_Proxyを要求し、サーバ側のBonDriver_Proxyも
								// 同じサーバに対して自分自身を要求する無限ループ状態だけのハズ
								// なお、STRICT_LOCKが定義してある場合は、そもそもデッドロックを
								// 起こすので、後者の状況は発生しない
								// 無限ループ状態に関しては放置
								// 無限ループ状態以外の場合は一応リストの最後まで検索してみて、
								// それでも見つからなかったらCreateBonDriver()をやらせてみる
							}
						}
					}
					if (!bFind)
					{
						if ((CreateBonDriver() != NULL) && (m_pIBon2 != NULL))
							makePacket(eCreateBonDriver, TRUE);
						else
						{
							makePacket(eCreateBonDriver, FALSE);
							m_Error.Set();
						}
					}
					else
						makePacket(eCreateBonDriver, TRUE);
				}
				else
					makePacket(eCreateBonDriver, TRUE);
				break;
			}

			case eOpenTuner:
			{
				::fprintf(stderr, "sock%d: OpenTuner\n", m_s);

				BOOL bFind = FALSE;
				{
#ifndef STRICT_LOCK
					LOCK(g_Lock);
#endif
					for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
						{
							if ((*it)->m_bTunerOpen)
							{
								bFind = TRUE;
								m_bTunerOpen = TRUE;
								break;
							}
						}
					}
				}
				if (!bFind)
					m_bTunerOpen = OpenTuner();
				makePacket(eOpenTuner, m_bTunerOpen);
				break;
			}

			case eCloseTuner:
			{
				::fprintf(stderr, "sock%d: CloseTuner\n", m_s);

				BOOL bFind = FALSE;
				{
#ifndef STRICT_LOCK
					LOCK(g_Lock);
#endif
					for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
						{
							if ((*it)->m_bTunerOpen)
							{
								bFind = TRUE;
								break;
							}
						}
					}
				}
				if (!bFind)
				{
					if (m_hTsRead)
					{
						m_pTsReaderArg->StopTsRead = TRUE;
						::pthread_join(m_hTsRead, NULL);
						delete m_pTsReaderArg;
					}
					CloseTuner();
				}
				else
				{
					if (m_hTsRead)
					{
#ifndef STRICT_LOCK
						LOCK(g_Lock);
#endif
						m_pTsReaderArg->TsLock.Enter();
						std::list<cProxyServer *>::iterator it = m_pTsReaderArg->TsReceiversList.begin();
						while (it != m_pTsReaderArg->TsReceiversList.end())
						{
							if (*it == this)
							{
								m_pTsReaderArg->TsReceiversList.erase(it);
								break;
							}
							++it;
						}
						m_pTsReaderArg->TsLock.Leave();

						// このインスタンスはチャンネル排他権を持っているか？
						if (m_bChannelLock == 0xff)
						{
							// 持っていた場合は、排他権取得待ちのインスタンスは存在しているか？
							if (m_pTsReaderArg->WaitExclusivePrivList.size() > 0)
							{
								// 存在する場合は、リスト先頭のインスタンスに排他権を引き継ぎ、リストから削除
								cProxyServer *p = m_pTsReaderArg->WaitExclusivePrivList.front();
								m_pTsReaderArg->WaitExclusivePrivList.pop_front();
								p->m_bChannelLock = 0xff;
							}
						}
						else
						{
							// 持っていない場合は、排他権取得待ちリストに自身が含まれているかもしれないので削除
							m_pTsReaderArg->WaitExclusivePrivList.remove(this);
						}

						// 可能性は低いがゼロではない…
						if (m_pTsReaderArg->TsReceiversList.empty())
						{
							m_pTsReaderArg->StopTsRead = TRUE;
							::pthread_join(m_hTsRead, NULL);
							delete m_pTsReaderArg;
						}
					}
				}
				m_bChannelLock = 0;
				m_hTsRead = 0;
				m_pTsReaderArg = NULL;
				m_bTunerOpen = FALSE;
				break;
			}

			case ePurgeTsStream:
			{
				if (m_hTsRead)
				{
					m_pTsReaderArg->TsLock.Enter();
					if (m_pTsReaderArg->TsReceiversList.size() <= 1)
					{
						PurgeTsStream();
						m_pTsReaderArg->pos = 0;
					}
					m_pTsReaderArg->TsLock.Leave();
					makePacket(ePurgeTsStream, TRUE);
				}
				else
					makePacket(ePurgeTsStream, FALSE);
				break;
			}

			case eRelease:
				m_Error.Set();
				break;

			case eEnumTuningSpace:
			{
				if (pPh->GetBodyLength() != sizeof(DWORD))
				{
					TCHAR c = 0;
					makePacket(eEnumTuningSpace, &c);
				}
				else
				{
					DWORD *pdw = (DWORD *)(pPh->m_pPacket->payload);
					LPCTSTR p = EnumTuningSpace(ntohl(*pdw));
					if (p)
						makePacket(eEnumTuningSpace, p);
					else
					{
						TCHAR c = 0;
						makePacket(eEnumTuningSpace, &c);
					}
				}
				break;
			}

			case eEnumChannelName:
			{
				if (pPh->GetBodyLength() != (sizeof(DWORD) * 2))
				{
					TCHAR c = 0;
					makePacket(eEnumChannelName, &c);
				}
				else
				{
					DWORD *pdw1 = (DWORD *)(pPh->m_pPacket->payload);
					DWORD *pdw2 = (DWORD *)(&(pPh->m_pPacket->payload[sizeof(DWORD)]));
					LPCTSTR p = EnumChannelName(ntohl(*pdw1), ntohl(*pdw2));
					if (p)
						makePacket(eEnumChannelName, p);
					else
					{
						TCHAR c = 0;
						makePacket(eEnumChannelName, &c);
					}
				}
				break;
			}

			case eSetChannel2:
			{
				::fprintf(stderr, "sock%d: SetChannel: [%d, %d]\n",
							m_s,
							ntohl(*(DWORD *)(pPh->m_pPacket->payload)),
							ntohl(*(DWORD *)(&(pPh->m_pPacket->payload[sizeof(DWORD)]))));

				if (pPh->GetBodyLength() != ((sizeof(DWORD) * 2) + sizeof(BYTE)))
					makePacket(eSetChannel2, (DWORD)0xff);
				else
				{
					m_bChannelLock = pPh->m_pPacket->payload[sizeof(DWORD) * 2];
					BOOL bLocked = FALSE;
					cProxyServer *pHavePriv = NULL;
					{
#ifndef STRICT_LOCK
						LOCK(g_Lock);
#endif
						for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
						{
							if (*it == this)
								continue;
							if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
							{
								if ((*it)->m_bChannelLock > m_bChannelLock)
									bLocked = TRUE;
								else if ((*it)->m_bChannelLock == 0xff)
								{
									// 対象チューナに対して優先度255のインスタンスが既にいる状態で、このインスタンスが
									// 要求している優先度も255の場合、このインスタンスの優先度を暫定的に254にする
									// (そうしないと、優先度255のインスタンスもチャンネル変更できなくなる為)
									m_bChannelLock = 0xfe;
									bLocked = TRUE;
									pHavePriv = *it;
								}
								if ((m_hTsRead == 0) && ((*it)->m_hTsRead != 0))
								{
									m_hTsRead = (*it)->m_hTsRead;
									m_pTsReaderArg = (*it)->m_pTsReaderArg;
									m_pTsReaderArg->TsLock.Enter();
									m_pTsReaderArg->TsReceiversList.push_back(this);
									m_pTsReaderArg->TsLock.Leave();
								}
							}
						}
						// このインスタンスの優先度が下げられた場合
						if (pHavePriv != NULL)
						{
							if (m_hTsRead)
							{
								// 排他権取得待ちリストにまだ自身が含まれていなければ追加
								BOOL bFind = FALSE;
								std::list<cProxyServer *>::iterator it = m_pTsReaderArg->WaitExclusivePrivList.begin();
								while (it != m_pTsReaderArg->WaitExclusivePrivList.end())
								{
									if (*it == this)
									{
										bFind = TRUE;
										break;
									}
									++it;
								}
								if (!bFind)
									m_pTsReaderArg->WaitExclusivePrivList.push_back(this);
							}
							else
							{
								// このインスタンスの優先度が下げられたが、排他権を持っているインスタンスへの配信が
								// 開始されていない場合は、そのインスタンスから排他権を奪う
								// こうする事が挙動として望ましいのかどうかは微妙だが、そもそもここに来るのは、
								// 当該インスタンスでのSetChannel()の失敗後、何もせずに接続だけ続けている状態であり、
								// 可能性としてはゼロではないものの、かなりのレアケースに限られるはず
								pHavePriv->m_bChannelLock = 0;
								m_bChannelLock = 0xff;
							}
						}
					}
					if (bLocked)
						makePacket(eSetChannel2, (DWORD)0x01);
					else
					{
						DWORD *pdw1 = (DWORD *)(pPh->m_pPacket->payload);
						DWORD *pdw2 = (DWORD *)(&(pPh->m_pPacket->payload[sizeof(DWORD)]));
						if (m_hTsRead)
							m_pTsReaderArg->TsLock.Enter();
						BOOL b = SetChannel(ntohl(*pdw1), ntohl(*pdw2));
						if (m_hTsRead)
						{
							// 一旦ロックを外すとチャンネル変更前のデータが送信されない事を保証できなくなる為、
							// チャンネル変更前のデータの破棄とCNRの更新指示はここで行う
							if (b)
							{
								m_pTsReaderArg->pos = 0;
								m_pTsReaderArg->ChannelChanged = TRUE;
							}
							m_pTsReaderArg->TsLock.Leave();
						}
						if (b)
						{
							makePacket(eSetChannel2, (DWORD)0x00);
							if (m_hTsRead == 0)
							{
#ifndef STRICT_LOCK
								// すぐ上で検索してるのになぜ再度検索するのかと言うと、同じBonDriverを要求している複数の
								// クライアントから、ほぼ同時のタイミングで最初のeSetChannel2をリクエストされた場合の為
								// eSetChannel2全体をまとめてロックすれば必要無くなるが、BonDriver_Proxyがロードされ、
								// それが自分自身に接続してきた場合デッドロックする事になる
								// なお、同様の理由でeCreateBonDriver, eOpenTuner, eCloseTunerのロックは実は不完全
								// しかし、自分自身への再帰接続を行わないならば完全なロックも可能
								// 実際の所、テスト用途以外で自分自身への再接続が必要になる状況と言うのはまず無いと
								// 思うので、STRICT_LOCKが定義してある場合は完全なロックを行う事にする
								// ただしそのかわりに、BonDriver_Proxyをロードし、そこからのプロキシチェーンのどこかで
								// 自分自身に再帰接続した場合はデッドロックとなるので注意
								BOOL bFind = FALSE;
								LOCK(g_Lock);
								for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
								{
									if (*it == this)
										continue;
									if (m_pIBon == (*it)->m_pIBon)
									{
										if ((*it)->m_hTsRead != 0)
										{
											bFind = TRUE;
											m_hTsRead = (*it)->m_hTsRead;
											m_pTsReaderArg = (*it)->m_pTsReaderArg;
											m_pTsReaderArg->TsLock.Enter();
											m_pTsReaderArg->TsReceiversList.push_back(this);
											m_pTsReaderArg->TsLock.Leave();
											break;
										}
									}
								}
								if (!bFind)
								{
#endif
									m_pTsReaderArg = new stTsReaderArg();
									m_pTsReaderArg->TsReceiversList.push_back(this);
									m_pTsReaderArg->pIBon = m_pIBon;
#ifdef HAVE_LIBARIBB25
									if (g_b25_enable)
										m_pTsReaderArg->b25.init();
#endif
									if (::pthread_create(&m_hTsRead, NULL, cProxyServer::TsReader, m_pTsReaderArg))
									{
										m_hTsRead = 0;
										delete m_pTsReaderArg;
										m_pTsReaderArg = NULL;
										m_Error.Set();
									}
#ifndef STRICT_LOCK
								}
#endif
							}
						}
						else
							makePacket(eSetChannel2, (DWORD)0xff);
					}
				}
				break;
			}

			case eGetTotalDeviceNum:
				makePacket(eGetTotalDeviceNum, GetTotalDeviceNum());
				break;

			case eGetActiveDeviceNum:
				makePacket(eGetActiveDeviceNum, GetActiveDeviceNum());
				break;

			case eSetLnbPower:
			{
				if (pPh->GetBodyLength() != sizeof(BYTE))
					makePacket(eSetLnbPower, FALSE);
				else
					makePacket(eSetLnbPower, SetLnbPower((BOOL)(pPh->m_pPacket->payload[0])));
				break;
			}

			case eGetClientInfo:
			{
				union {
					sockaddr_storage ss;
					sockaddr_in si4;
					sockaddr_in6 si6;
				};
				char addr[INET6_ADDRSTRLEN], buf[2048], info[4096], *p, *exinfo;
				int port, len, num = 0;
				size_t left, size;
				socklen_t slen;
				p = info;
				p[0] = '\0';
				left = size = sizeof(info);
				exinfo = NULL;
				std::list<cProxyServer *>::iterator it = g_InstanceList.begin();
				while (it != g_InstanceList.end())
				{
					slen = sizeof(ss);
					if (::getpeername((*it)->m_s, (sockaddr *)&ss, &slen) == 0)
					{
						if (ss.ss_family == AF_INET)
						{
							// IPv4
							::inet_ntop(AF_INET, &(si4.sin_addr), addr, sizeof(addr));
							port = ntohs(si4.sin_port);
						}
						else
						{
							// IPv6
							::inet_ntop(AF_INET6, &(si6.sin6_addr), addr, sizeof(addr));
							port = ntohs(si6.sin6_port);
						}
					}
					else
					{
						::strcpy(addr, "unknown host...");
						port = 0;
					}
					len = ::sprintf(buf, "%02d: [%s]:[%d] / [%s]\n", num, addr, port, (*it)->m_strBonDriver);
					if ((size_t)len >= left)
					{
						left += size;
						size *= 2;
						if (exinfo != NULL)
						{
							char *bp = exinfo;
							exinfo = new char[size];
							::strcpy(exinfo, bp);
							delete[] bp;
						}
						else
						{
							exinfo = new char[size];
							::strcpy(exinfo, info);
						}
						p = exinfo + ::strlen(exinfo);
					}
					::strcpy(p, buf);
					p += len;
					left -= len;
					num++;
					++it;
				}
				if (exinfo != NULL)
				{
					size = (p - exinfo) + 1;
					p = exinfo;
				}
				else
				{
					size = (p - info) + 1;
					p = info;
				}
				cPacketHolder *ph = new cPacketHolder(eGetClientInfo, size);
				::memcpy(ph->m_pPacket->payload, p, size);
				m_fifoSend.Push(ph);
				if (exinfo != NULL)
					delete[] exinfo;
				break;
			}

			default:
				break;
			}
			delete pPh;
			break;
		}

		default:
			// 何かのエラー
			m_Error.Set();
			goto end;
		}
	}
end:
	::pthread_join(hThread[0], NULL);
	::pthread_join(hThread[1], NULL);
	return 0;
}

int cProxyServer::ReceiverHelper(char *pDst, DWORD left)
{
	int len, ret;
	fd_set rd;
	timeval tv;

	while (left > 0)
	{
		if (m_Error.IsSet())
			return -1;

		FD_ZERO(&rd);
		FD_SET(m_s, &rd);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		if ((len = ::select((int)(m_s + 1), &rd, NULL, NULL, &tv)) == SOCKET_ERROR)
		{
			ret = -2;
			goto err;
		}

		if (len == 0)
			continue;

		if ((len = ::recv(m_s, pDst, left, 0)) <= 0)
		{
			ret = -3;
			goto err;
		}
		left -= len;
		pDst += len;
	}
	return 0;
err:
	m_Error.Set();
	return ret;
}

void *cProxyServer::Receiver(LPVOID pv)
{
	cProxyServer *pProxy = static_cast<cProxyServer *>(pv);
	DWORD left, &ret = pProxy->m_tRet;
	char *p;
	cPacketHolder *pPh = NULL;

	for (;;)
	{
		pPh = new cPacketHolder(16);
		left = sizeof(stPacketHead);
		p = (char *)&(pPh->m_pPacket->head);
		if (pProxy->ReceiverHelper(p, left) != 0)
		{
			ret = 201;
			goto end;
		}

		if (!pPh->IsValid())
		{
			pProxy->m_Error.Set();
			ret = 202;
			goto end;
		}

		left = pPh->GetBodyLength();
		if (left == 0)
		{
			pProxy->m_fifoRecv.Push(pPh);
			continue;
		}

		if (left > 16)
		{
			if (left > 512)
			{
				pProxy->m_Error.Set();
				ret = 203;
				goto end;
			}
			cPacketHolder *pTmp = new cPacketHolder(left);
			pTmp->m_pPacket->head = pPh->m_pPacket->head;
			delete pPh;
			pPh = pTmp;
		}

		p = (char *)(pPh->m_pPacket->payload);
		if (pProxy->ReceiverHelper(p, left) != 0)
		{
			ret = 204;
			goto end;
		}

		pProxy->m_fifoRecv.Push(pPh);
	}
end:
	delete pPh;
	return &ret;
}

void cProxyServer::makePacket(enumCommand eCmd, BOOL b)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(BYTE));
	p->m_pPacket->payload[0] = (BYTE)b;
	m_fifoSend.Push(p);
}

void cProxyServer::makePacket(enumCommand eCmd, DWORD dw)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(DWORD));
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos = htonl(dw);
	m_fifoSend.Push(p);
}

void cProxyServer::makePacket(enumCommand eCmd, LPCTSTR str)
{
	int i;
	for (i = 0; str[i]; i++);
	register size_t size = (i + 1) * sizeof(TCHAR);
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	::memcpy(p->m_pPacket->payload, str, size);
	m_fifoSend.Push(p);
}

void cProxyServer::makePacket(enumCommand eCmd, BYTE *pSrc, DWORD dwSize, float fSignalLevel)
{
	register size_t size = (sizeof(DWORD) * 2) + dwSize;
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	union {
		DWORD dw;
		float f;
	} u;
	u.f = fSignalLevel;
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos++ = htonl(dwSize);
	*pos++ = htonl(u.dw);
	if (dwSize > 0)
		::memcpy(pos, pSrc, dwSize);
	m_fifoSend.Push(p);
}

void *cProxyServer::Sender(LPVOID pv)
{
	cProxyServer *pProxy = static_cast<cProxyServer *>(pv);
	DWORD &ret = pProxy->m_tRet;
	cEvent *h[2] = { &(pProxy->m_Error), pProxy->m_fifoSend.GetEventHandle() };
	for (;;)
	{
		DWORD dwRet = cEvent::MultipleWait(2, h);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			ret = 101;
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			cPacketHolder *pPh = NULL;
			pProxy->m_fifoSend.Pop(&pPh);
			int left = (int)pPh->m_Size;
			char *p = (char *)(pPh->m_pPacket);
			while (left > 0)
			{
				int len = ::send(pProxy->m_s, p, left, 0);
				if (len == SOCKET_ERROR)
				{
					pProxy->m_Error.Set();
					break;
				}
				left -= len;
				p += len;
			}
			delete pPh;
			break;
		}

		default:
			// 何かのエラー
			pProxy->m_Error.Set();
			ret = 102;
			goto end;
		}
	}
end:
	return &ret;
}

void *cProxyServer::TsReader(LPVOID pv)
{
	stTsReaderArg *pArg = static_cast<stTsReaderArg *>(pv);
	IBonDriver *pIBon = pArg->pIBon;
	volatile BOOL &StopTsRead = pArg->StopTsRead;
	volatile BOOL &ChannelChanged = pArg->ChannelChanged;
	DWORD &pos = pArg->pos;
	std::list<cProxyServer *> &TsReceiversList = pArg->TsReceiversList;
	cCriticalSection &TsLock = pArg->TsLock;
	DWORD dwSize, dwRemain, now, before = 0;
	float fSignalLevel = 0;
	const DWORD TsPacketBufSize = g_TsPacketBufSize;
	BYTE *pBuf, *pTsBuf = new BYTE[TsPacketBufSize];
	timeval tv;
	timespec ts;

	ts.tv_sec = 0;
	ts.tv_nsec = WAIT_TIME * 1000 * 1000;

	// TS読み込みループ
	while (!StopTsRead)
	{
		dwSize = dwRemain = 0;
		{
			LOCK(TsLock);
			::gettimeofday(&tv, NULL);
			now = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
			if (((now - before) >= 1000) || ChannelChanged)
			{
#ifdef HAVE_LIBARIBB25
				if (ChannelChanged)
					pArg->b25.reset();
#endif
				fSignalLevel = pIBon->GetSignalLevel();
				before = now;
				ChannelChanged = FALSE;
			}
			if (pIBon->GetTsStream(&pBuf, &dwSize, &dwRemain) && (dwSize != 0))
			{
#ifdef HAVE_LIBARIBB25
				if (g_b25_enable)
				{
					pArg->b25.decode(pBuf, dwSize, &pBuf, &dwSize);
					if (dwSize == 0)
						goto next;
				}
#endif
				if ((pos + dwSize) < TsPacketBufSize)
				{
					::memcpy(&pTsBuf[pos], pBuf, dwSize);
					pos += dwSize;
					if (dwRemain == 0)
					{
						for (std::list<cProxyServer *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
							(*it)->makePacket(eGetTsStream, pTsBuf, pos, fSignalLevel);
						pos = 0;
					}
				}
				else
				{
					DWORD left, dwLen = TsPacketBufSize - pos;
					::memcpy(&pTsBuf[pos], pBuf, dwLen);
					for (std::list<cProxyServer *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
						(*it)->makePacket(eGetTsStream, pTsBuf, TsPacketBufSize, fSignalLevel);
					left = dwSize - dwLen;
					pBuf += dwLen;
					while (left >= TsPacketBufSize)
					{
						for (std::list<cProxyServer *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
							(*it)->makePacket(eGetTsStream, pBuf, TsPacketBufSize, fSignalLevel);
						left -= TsPacketBufSize;
						pBuf += TsPacketBufSize;
					}
					if (left != 0)
					{
						if (dwRemain == 0)
						{
							for (std::list<cProxyServer *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
								(*it)->makePacket(eGetTsStream, pBuf, left, fSignalLevel);
							left = 0;
						}
						else
							::memcpy(pTsBuf, pBuf, left);
					}
					pos = left;
				}
			}
		}
#ifdef HAVE_LIBARIBB25
	next:
#endif
		if (dwRemain == 0)
			::nanosleep(&ts, NULL);
	}
	delete[] pTsBuf;
	return NULL;
}

BOOL cProxyServer::SelectBonDriver(LPCSTR p)
{
	HMODULE hModule = NULL;
	BOOL bLoaded = FALSE;
	for (std::list<stLoadedDriver *>::iterator it = g_LoadedDriverList.begin(); it != g_LoadedDriverList.end(); ++it)
	{
		if (::strcmp(p, (*it)->strBonDriver) == 0)
		{
			hModule = (*it)->hModule;
			bLoaded = TRUE;
			break;
		}
	}
	if (hModule == NULL)
	{
		hModule = ::dlopen(p, RTLD_LAZY);
		if (hModule == NULL)
			return FALSE;
#ifdef DEBUG
		::fprintf(stderr, "[%s] loaded\n", p);
#endif
	}

	m_hModule = hModule;

	if (g_DisableUnloadBonDriver && !bLoaded)
	{
		stLoadedDriver *pLd = new stLoadedDriver;
		::strcpy(pLd->strBonDriver, p);	// stLoadedDriver::strBonDriverのサイズはProxyServer::m_strBonDriverと同じ
		pLd->hModule = hModule;
		g_LoadedDriverList.push_back(pLd);
	}

	return TRUE;
}

IBonDriver *cProxyServer::CreateBonDriver()
{
	if (m_hModule)
	{
		char *err;
		::dlerror();
		IBonDriver *(*f)() = (IBonDriver *(*)())::dlsym(m_hModule, "CreateBonDriver");
		if ((err = ::dlerror()) == NULL)
		{
			try { m_pIBon = f(); }
			catch (...) {}
			if (m_pIBon)
			{
				m_pIBon2 = dynamic_cast<IBonDriver2 *>(m_pIBon);
				m_pIBon3 = dynamic_cast<IBonDriver3 *>(m_pIBon);
			}
		}
		else
			::fprintf(stderr, "CreateBonDriver(): %s\n", err);
	}
	return m_pIBon;
}

const BOOL cProxyServer::OpenTuner(void)
{
	BOOL b = FALSE;
	if (m_pIBon)
		b = m_pIBon->OpenTuner();
	return b;
}

void cProxyServer::CloseTuner(void)
{
	if (m_pIBon)
		m_pIBon->CloseTuner();
}

void cProxyServer::PurgeTsStream(void)
{
	if (m_pIBon)
		m_pIBon->PurgeTsStream();
}

LPCTSTR cProxyServer::EnumTuningSpace(const DWORD dwSpace)
{
	LPCTSTR pStr = NULL;
	if (m_pIBon2)
		pStr = m_pIBon2->EnumTuningSpace(dwSpace);
	return pStr;
}

LPCTSTR cProxyServer::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	LPCTSTR pStr = NULL;
	if (m_pIBon2)
		pStr = m_pIBon2->EnumChannelName(dwSpace, dwChannel);
	return pStr;
}

const BOOL cProxyServer::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	BOOL b = FALSE;
	if (m_pIBon2)
		b = m_pIBon2->SetChannel(dwSpace, dwChannel);
	return b;
}

const DWORD cProxyServer::GetTotalDeviceNum(void)
{
	DWORD d = 0;
	if (m_pIBon3)
		d = m_pIBon3->GetTotalDeviceNum();
	return d;
}

const DWORD cProxyServer::GetActiveDeviceNum(void)
{
	DWORD d = 0;
	if (m_pIBon3)
		d = m_pIBon3->GetActiveDeviceNum();
	return d;
}

const BOOL cProxyServer::SetLnbPower(const BOOL bEnable)
{
	BOOL b = FALSE;
	if (m_pIBon3)
		b = m_pIBon3->SetLnbPower(bEnable);
	return b;
}

static int Listen(char *host, char *port)
{
	addrinfo hints, *results, *rp;
	SOCKET lsock, csock;

	::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
	if (::getaddrinfo(host, port, &hints, &results) != 0)
	{
		hints.ai_flags = AI_PASSIVE;
		if (::getaddrinfo(host, port, &hints, &results) != 0)
			return 1;
	}

	for (rp = results; rp != NULL; rp = rp->ai_next)
	{
		lsock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (lsock == INVALID_SOCKET)
			continue;

		BOOL reuse = TRUE;
		::setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

		if (::bind(lsock, rp->ai_addr, (int)(rp->ai_addrlen)) != SOCKET_ERROR)
			break;

		::close(lsock);
	}
	::freeaddrinfo(results);
	if (rp == NULL)
		return 2;

	if (::listen(lsock, 4) == SOCKET_ERROR)
	{
		::close(lsock);
		return 3;
	}

	for (;;)
	{
		struct sockaddr_in peer_sin;
		unsigned int len;
		len = sizeof(peer_sin);
		csock = ::accept(lsock, (struct sockaddr *)&peer_sin, &len);
		if (csock == INVALID_SOCKET)
		{
			if ((errno == EINTR) && g_bStop)
				break;

			continue;
		}

		struct hostent *peer_host;
		const char *h_name;
		peer_host = gethostbyaddr((char *)&peer_sin.sin_addr.s_addr, sizeof(peer_sin.sin_addr), AF_INET);
		if (peer_host == NULL)
			h_name = "NONAME";
		else
			h_name = peer_host->h_name;
		::fprintf(stderr,"sock%d: connect from: %s [%s] port %d\n", csock, h_name, inet_ntoa(peer_sin.sin_addr), ntohs(peer_sin.sin_port));

		cProxyServer *pProxy = new cProxyServer();
		pProxy->setSocket(csock);

		pthread_t ht;
		pthread_attr_t attr;
		if (::pthread_attr_init(&attr))
			goto retry;
		if (::pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
			goto retry;
		if (::pthread_create(&ht, &attr, cProxyServer::Reception, pProxy) == 0)
			continue;
	retry:
		delete pProxy;
	}

	return 0;	// ここには来ない
}

}

int main(int argc, char *argv[])
{
	if (BonDriverProxy::Init(argc, argv) != 0)
	{
#ifdef HAVE_LIBARIBB25
		fprintf(stderr, "usage: %s [--b25 [--strip] [--emm] [--round N]] "
				"address port (packet_fifo_size tspacket_bufsize)\ne.g. $ %s 192.168.0.100 1192\n", argv[0],argv[0]);
#else
		fprintf(stderr, "usage: %s address port (packet_fifo_size tspacket_bufsize)\ne.g. $ %s 192.168.0.100 1192\n", argv[0],argv[0]);
#endif
		return 0;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGPIPE, &sa, NULL))
	{
		perror("sigaction1");
		return -2;
	}

	sa.sa_handler = BonDriverProxy::Handler;
	if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL))
	{
		perror("sigaction2 or 15");
		return -3;
	}

	int ret = BonDriverProxy::Listen(BonDriverProxy::g_Host, BonDriverProxy::g_Port);

	BonDriverProxy::g_Lock.Enter();
	BonDriverProxy::CleanUp();
	BonDriverProxy::g_Lock.Leave();

	return ret;
}
