﻿#include "learning_tools.h"

#if defined (EVAL_LEARN)

#if defined(_OPENMP)
#include <omp.h>
#endif
#include "../misc.h"

namespace EvalLearningTools
{

	// --- static variables

	double Weight::eta;
	double Weight::eta1;
	double Weight::eta2;
	double Weight::eta3;
	u64 Weight::eta1_epoch;
	u64 Weight::eta2_epoch;

#if defined (SGD_UPDATE)
	AsyncPRNG Weight::prng;
#endif

	// --- tables

	// あるBonaPieceを相手側から見たときの値
	// BONA_PIECE_INITが-1なので符号型で持つ必要がある。
	// KPPTを拡張しても当面、BonaPieceが2^15を超えることはないのでs16で良しとする。
	s16 inv_piece_[Eval::fe_end];

	// 盤面上のあるBonaPieceをミラーした位置にあるものを返す。
	s16 mir_piece_[Eval::fe_end];

	std::vector<bool> min_index_flag;


	// --- methods

	// あるBonaPieceを相手側から見たときの値を返す
	Eval::BonaPiece inv_piece(Eval::BonaPiece p) { return (Eval::BonaPiece)inv_piece_[p]; }

	// 盤面上のあるBonaPieceをミラーした位置にあるものを返す。
	Eval::BonaPiece mir_piece(Eval::BonaPiece p) { return (Eval::BonaPiece)mir_piece_[p]; }

	std::function<void()> mir_piece_init_function;


	// --- 個別のテーブルごとの初期化

	void init_min_index_flag()
	{
		// mir_piece、inv_pieceの初期化が終わっていなければならない。
		ASSERT_LV1(mir_piece(Eval::f_hand_pawn) == Eval::f_hand_pawn);

		// 次元下げ用フラグ配列の初期化
		u64 size = KPP::max_index();
		min_index_flag.resize(size);

#pragma omp parallel
		{
#if defined(_OPENMP)
			// Windows環境下でCPUが２つあるときに、論理64コアまでしか使用されないのを防ぐために
			// ここで明示的にCPUに割り当てる
			int thread_index = omp_get_thread_num();    // 自分のthread numberを取得
			WinProcGroup::bindThisThread(thread_index);
#endif

#pragma omp parallel for schedule(dynamic,20000)

			for (s64 index_ = 0; index_ < (s64)size; ++index_)
			{
				// OpenMPの制約からループ変数は符号型でないといけないらしいのだが、
				// さすがに使いにくい。
				u64 index = (u64)index_;

				if (KK::is_ok(index))
				{
					// indexからの変換と逆変換によって元のindexに戻ることを確認しておく。
					// 起動時に1回しか実行しない処理なのでASSERT_LV1で書いておく。
					ASSERT_LV1(KK::fromIndex(index).toIndex() == index);

					KK a[KK_LOWER_COUNT];
					KK::fromIndex(index).toLowerDimensions(a);

					// 次元下げの1つ目の要素が元のindexと同一であることを確認しておく。
					ASSERT_LV1(a[0].toIndex() == index);

					u64 min_index = UINT64_MAX;
					for (auto& e : a)
						min_index = std::min(min_index, e.toIndex());
					min_index_flag[index] = (min_index == index);
				}
				else if (KKP::is_ok(index))
				{
					ASSERT_LV1(KKP::fromIndex(index).toIndex() == index);

					KKP x = KKP::fromIndex(index);
					KKP a[KKP_LOWER_COUNT];
					x.toLowerDimensions(a);

					ASSERT_LV1(a[0].toIndex() == index);

					u64 min_index = UINT64_MAX;
					for (auto& e : a)
						min_index = std::min(min_index, e.toIndex());
					min_index_flag[index] = (min_index == index);
				}
				else if (KPP::is_ok(index))
				{
					ASSERT_LV1(KPP::fromIndex(index).toIndex() == index);

					KPP x = KPP::fromIndex(index);
					KPP a[KPP_LOWER_COUNT];
					x.toLowerDimensions(a);

					ASSERT_LV1(a[0].toIndex() == index);

					u64 min_index = UINT64_MAX;
					for (auto& e : a)
						min_index = std::min(min_index, e.toIndex());
					min_index_flag[index] = (min_index == index);
				}
				else
				{
					ASSERT_LV3(false);
				}
			}
		}
	}

	using namespace Eval;
	void init_mir_inv_tables()
	{
		// mirrorとinverseのテーブルの初期化。

			// fとeとの交換
		int t[] = {
			f_hand_pawn - 1    , e_hand_pawn - 1   ,
			f_hand_lance - 1   , e_hand_lance - 1  ,
			f_hand_knight - 1  , e_hand_knight - 1 ,
			f_hand_silver - 1  , e_hand_silver - 1 ,
			f_hand_gold - 1    , e_hand_gold - 1   ,
			f_hand_bishop - 1  , e_hand_bishop - 1 ,
			f_hand_rook - 1    , e_hand_rook - 1   ,
			f_pawn             , e_pawn            ,
			f_lance            , e_lance           ,
			f_knight           , e_knight          ,
			f_silver           , e_silver          ,
			f_gold             , e_gold            ,
			f_bishop           , e_bishop          ,
			f_horse            , e_horse           ,
			f_rook             , e_rook            ,
			f_dragon           , e_dragon          ,
		};

		// 未初期化の値を突っ込んでおく。
		for (BonaPiece p = BONA_PIECE_ZERO; p < fe_end; ++p)
		{
			inv_piece_[p] = BONA_PIECE_NOT_INIT;

			// mirrorは手駒に対しては機能しない。元の値を返すだけ。
			mir_piece_[p] = (p < f_pawn) ? p : BONA_PIECE_NOT_INIT;
		}

		for (BonaPiece p = BONA_PIECE_ZERO; p < fe_end; ++p)
		{
			for (int i = 0; i < 32 /* t.size() */; i += 2)
			{
				if (t[i] <= p && p < t[i + 1])
				{
					Square sq = (Square)(p - t[i]);

					// 見つかった!!
					BonaPiece q = (p < fe_hand_end) ? BonaPiece(sq + t[i + 1]) : (BonaPiece)(Inv(sq) + t[i + 1]);
					inv_piece_[p] = q;
					inv_piece_[q] = p;

					/*
					ちょっとトリッキーだが、pに関して盤上の駒は
					p >= fe_hand_end
					のとき。

					このpに対して、nを整数として(上のコードのiは偶数しかとらない)、
					a)  t[2n + 0] <= p < t[2n + 1] のときは先手の駒
					b)  t[2n + 1] <= p < t[2n + 2] のときは後手の駒
					　である。

					 ゆえに、a)の範囲にあるpをq = Inv(p-t[2n+0]) + t[2n+1] とすると180度回転させた升にある後手の駒となる。
					 そこでpとqをswapさせてinv_piece[ ]を初期化してある。
					 */

					 // 手駒に関してはmirrorなど存在しない。
					if (p < fe_hand_end)
						continue;

					BonaPiece r1 = (BonaPiece)(Mir(sq) + t[i]);
					mir_piece_[p] = r1;
					mir_piece_[r1] = p;

					BonaPiece p2 = (BonaPiece)(sq + t[i + 1]);
					BonaPiece r2 = (BonaPiece)(Mir(sq) + t[i + 1]);
					mir_piece_[p2] = r2;
					mir_piece_[r2] = p2;

					break;
				}
			}
		}

		if (mir_piece_init_function)
			mir_piece_init_function();

		for (BonaPiece p = BONA_PIECE_ZERO; p < fe_end; ++p)
		{
			// 未初期化のままになっている。上のテーブルの初期化コードがおかしい。
			ASSERT_LV1(mir_piece_[p] != BONA_PIECE_NOT_INIT && mir_piece_[p] < fe_end);
			ASSERT_LV1(inv_piece_[p] != BONA_PIECE_NOT_INIT && inv_piece_[p] < fe_end);

			// mirとinvは、2回適用したら元の座標に戻る。
			ASSERT_LV1(mir_piece_[mir_piece_[p]] == p);
			ASSERT_LV1(inv_piece_[inv_piece_[p]] == p);
		}

#if 0
		// 評価関数のミラーをしても大丈夫であるかの事前検証
		// 値を書き込んだときにassertionがあるので、ミラーしてダメである場合、
		// そのassertに引っかかるはず。

		// AperyのWCSC26の評価関数、kppのp1==0とかp1==20(後手の0枚目の歩)とかの
		// ところにゴミが入っていて、これを回避しないとassertに引っかかる。

		std::unordered_set<BonaPiece> s;
		vector<int> a = {
			f_hand_pawn - 1,e_hand_pawn - 1,
			f_hand_lance - 1, e_hand_lance - 1,
			f_hand_knight - 1, e_hand_knight - 1,
			f_hand_silver - 1, e_hand_silver - 1,
			f_hand_gold - 1, e_hand_gold - 1,
			f_hand_bishop - 1, e_hand_bishop - 1,
			f_hand_rook - 1, e_hand_rook - 1,
		};
		for (auto b : a)
			s.insert((BonaPiece)b);

		// さらに出現しない升の盤上の歩、香、桂も除外(Aperyはここにもゴミが入っている)
		for (Rank r = RANK_1; r <= RANK_2; ++r)
			for (File f = FILE_1; f <= FILE_9; ++f)
			{
				if (r == RANK_1)
				{
					// 1段目の歩
					BonaPiece b1 = BonaPiece(f_pawn + (f | r));
					s.insert(b1);
					s.insert(inv_piece[b1]);

					// 1段目の香
					BonaPiece b2 = BonaPiece(f_lance + (f | r));
					s.insert(b2);
					s.insert(inv_piece[b2]);
				}

				// 1,2段目の桂
				BonaPiece b = BonaPiece(f_knight + (f | r));
				s.insert(b);
				s.insert(inv_piece[b]);
			}

		cout << "\nchecking kpp_write()..";
		for (auto sq : SQ)
		{
			cout << sq << ' ';
			for (BonaPiece p1 = BONA_PIECE_ZERO; p1 < fe_end; ++p1)
				for (BonaPiece p2 = BONA_PIECE_ZERO; p2 < fe_end; ++p2)
					if (!s.count(p1) && !s.count(p2))
						kpp_write(sq, p1, p2, kpp[sq][p1][p2]);
		}
		cout << "\nchecking kkp_write()..";

		for (auto sq1 : SQ)
		{
			cout << sq1 << ' ';
			for (auto sq2 : SQ)
				for (BonaPiece p1 = BONA_PIECE_ZERO; p1 < fe_end; ++p1)
					if (!s.count(p1))
						kkp_write(sq1, sq2, p1, kkp[sq1][sq2][p1]);
		}
		cout << "..done!" << endl;
#endif
	}

	void learning_tools_unit_test_kpp()
	{

		// KPPの三角配列化にバグがないかテストする
		// k-p0-p1のすべての組み合わせがきちんとKPPの扱う対象になっていかと、そのときの次元下げが
		// 正しいかを判定する。

		std::vector<bool> f;
		f.resize(KPP::max_index() - KPP::min_index());

		for(auto k = SQ_ZERO ; k < SQ_NB ; ++k)
			for(auto p0 = BonaPiece::BONA_PIECE_ZERO; p0 < fe_end ; ++p0)
				for (auto p1 = BonaPiece::BONA_PIECE_ZERO; p1 < fe_end; ++p1)
				{
					KPP kpp_org(k,p0,p1);
					KPP kpp0;
					KPP kpp1(Mir(k), mir_piece(p0), mir_piece(p1));
					KPP kpp_array[2];

					auto index = kpp_org.toIndex();
					ASSERT_LV3(KPP::is_ok(index));

					kpp0 = KPP::fromIndex(index);

					//if (kpp0 != kpp_org)
					//	std::cout << "index = " << index << "," << kpp_org << "," << kpp0 << std::endl;

					kpp0.toLowerDimensions(kpp_array);

					ASSERT_LV3(kpp_array[0] == kpp0);
					ASSERT_LV3(kpp0 == kpp_org);
					ASSERT_LV3(kpp_array[1] == kpp1);

					auto index2 = kpp1.toIndex();
					f[index - KPP::min_index()] = f[index2-KPP::min_index()] = true;
				}

		// 抜けてるindexがなかったかの確認。
		for(size_t index = 0 ; index < f.size(); index++)
			if (!f[index])
			{
				std::cout << index << KPP::fromIndex(index + KPP::min_index()) <<  std::endl;
			}
	}

	void learning_tools_unit_test_kppp()
	{
		// KPPPの計算に抜けがないかをテストする

		u64 min_index = KPPP::min_index();
		u64 max_index = KPPP::max_index();

		// 最後の要素の確認。
		//KPPP x = KPPP::fromIndex(max_index-1);
		//std::cout << x << std::endl;

		for (u64 index = min_index; index < max_index; ++index)
		{
			KPPP x = KPPP::fromIndex(index);
			//std::cout << x << std::endl;

#if 0
			if ((index % 10000000) == 0)
				std::cout << "index = " << index << std::endl;

			// index = 9360000000
			//	done.

			if (x.toIndex() != index)
			{
				std::cout << "assertion failed , index = " << index << std::endl;
			}
#endif

			ASSERT(x.toIndex() == index);
		}

	}

	// このEvalLearningTools全体の初期化
	void init()
	{
		// 初期化は、起動後1回限りで良いのでそのためのフラグ。
		static bool first = true;

		if (first)
		{
			std::cout << "EvalLearningTools init..";

			// mir_piece()とinv_piece()を利用可能にする。
			// このあとmin_index_flagの初期化を行なうが、そこが
			// これに依存しているので、こちらを先に行なう必要がある。
			init_mir_inv_tables();

			//learning_tools_unit_test_kpp();
			//learning_tools_unit_test_kppp();
			// UnitTestを実行するの最後でも良いのだが、init_min_index_flag()にとても時間がかかるので
			// デバッグ時はこのタイミングで行いたい。

			init_min_index_flag();

			std::cout << "done." << std::endl;

			first = false;
		}
	}
}

#endif
