/******************************************************************************
  Copyright (c) 2007-2018, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include "dpml_ux.h"

#define NUM_FRAC_BITS 7

typedef struct {
	float a, b;
	double c;
} SQRT_COEF_STRUCT;

const SQRT_COEF_STRUCT D_SQRT_TABLE_NAME[(1<<(NUM_FRAC_BITS+1))] = {


/*
**	a*x^2 + b*x + c ~= sqrt(1.0/x),		5e-1 <= x < 1.0
*/
{	2.100767374,	-3.5149509907,	2.6464972078561099359	},
{	2.0604462624,	-3.4743154049,	2.6362590670696124814	},
{	2.0212004185,	-3.4344568253,	2.6261388339097704914	},
{	1.9829931259,	-3.3953547478,	2.6161343671734754664	},
{	1.9457894564,	-3.3569889069,	2.6062432760972451132	},
{	1.9095555544,	-3.319340229,	2.596463590697826963	},
{	1.8742593527,	-3.2823901176,	2.5867931995248653203	},
{	1.8398698568,	-3.2461204529,	2.5772300318703730094	},
{	1.8063572645,	-3.2105138302,	2.5677721468918503548	},
{	1.7736930847,	-3.1755533218,	2.5584175687659680947	},
{	1.7418498993,	-3.1412229538,	2.5491646043746129315	},
{	1.710801363,	-3.1075065136,	2.5400111974361759497	},
{	1.6805220842,	-3.0743889809,	2.5309557295054293075	},
{	1.6509878635,	-3.0418555737,	2.5219964300360217983	},
{	1.6221753359,	-3.0098922253,	2.513131738451644744	},
{	1.5940617323,	-2.9784843922,	2.5043597132692148449	},
{	1.5666255951,	-2.9476189613,	2.4956788648540670586	},
{	1.5398460627,	-2.9172832966,	2.4870878496565907177	},
{	1.5137029886,	-2.8874640465,	2.4785847509253505943	},
{	1.4881771803,	-2.8581497669,	2.4701684863694106596	},
{	1.4632499218,	-2.8293278217,	2.4618371919667651739	},
{	1.4389033318,	-2.8009872437,	2.453589741956652739	},
{	1.4151201248,	-2.7731165886,	2.4454245884260308602	},
{	1.3918836117,	-2.7457051277,	2.4373404486694877623	},
{	1.3691778183,	-2.7187423706,	2.4293359837450848466	},
{	1.3469872475,	-2.6922178268,	2.421409733797203716	},
{	1.3252968788,	-2.6661219597,	2.4135606868112681467	},
{	1.3040924072,	-2.640444994,	2.4057874781650356784	},
{	1.2833598852,	-2.615177393,	2.3980887982484154078	},
{	1.2630858421,	-2.5903103352,	2.3904636407379154103	},
{	1.2432574034,	-2.5658347607,	2.3829106727614295971	},
{	1.223862052,	-2.5417423248,	2.3754289062314430029	},
{	1.2048876286,	-2.518024683,	2.368017258199012865	},
{	1.1863225698,	-2.4946734905,	2.3606744506319210228	},
{	1.168155551,	-2.471681118,	2.3533995971537280486	},
{	1.1503756046,	-2.4490396976,	2.3461915565573080325	},
{	1.1329722404,	-2.4267418385,	2.3390493327072075284	},
{	1.1159352064,	-2.4047801495,	2.3319718662505561095	},
{	1.0992547274,	-2.3831479549,	2.3249583958105440118	},
{	1.0829212666,	-2.3618381023,	2.3180077847190338448	},
{	1.0669255257,	-2.3408436775,	2.3111189790671896735	},
{	1.0512586832,	-2.3201589584,	2.3042915357589123534	},
{	1.0359119177,	-2.2997765541,	2.2975239420187385902	},
{	1.0208771229,	-2.2796912193,	2.2908158197682714623	},
{	1.0061459541,	-2.2598962784,	2.2841659744155985018	},
{	9.917107224e-1,	-2.2403864861,	2.2775739037802745675	},
{	9.775637984e-1,	-2.2211556435,	2.2710384627191028825	},
{	9.636977911e-1,	-2.202198267,	2.2645589051141198363	},
{	9.501055479e-1,	-2.1835091114,	2.2581345647863715557	},
{	9.367802143e-1,	-2.1650829315,	2.2517646617865273394	},
{	9.23715055e-1,	-2.1469142437,	2.2454482169670996322	},
{	9.10903573e-1,	-2.1289982796,	2.2391846560382706227	},
{	8.9833951e-1,	-2.1113302708,	2.2329733145353977077	},
{	8.860167265e-1,	-2.0939052105,	2.2268133239115151086	},
{	8.739293218e-1,	-2.0767185688,	2.2207040546088282522	},
{	8.620714545e-1,	-2.0597655773,	2.2146446995886185717	},
{	8.504377007e-1,	-2.0430421829,	2.2086347711853326671	},
{	8.390225172e-1,	-2.0265438557,	2.2026735227736358976	},
{	8.278207183e-1,	-2.010266304,	2.1967602123284747298	},
{	8.16827178e-1,	-1.9942055941,	2.1908943455926404106	},
{	8.060369492e-1,	-1.9783575535,	2.18507517925991311	},
{	7.954452038e-1,	-1.9627183676,	2.179302186449165691	},
{	7.850472927e-1,	-1.9472841024,	2.1735746737563844612	},
{	7.748387456e-1,	-1.9320510626,	2.1678920425681950004	},
{	7.648150325e-1,	-1.9170156717,	2.1622538349306307204	},
{	7.549719214e-1,	-1.9021741152,	2.1566592642242442719	},
{	7.453052402e-1,	-1.8875231743,	2.1511079745610480496	},
{	7.358109951e-1,	-1.873059392,	2.145599345037042738	},
{	7.264851928e-1,	-1.8587793112,	2.140132769235723434	},
{	7.173240185e-1,	-1.8446798325,	2.1347078258028021738	},
{	7.083238363e-1,	-1.8307577372,	2.1293239113956808838	},
{	6.994808912e-1,	-1.8170098066,	2.1239805078667056155	},
{	6.907917261e-1,	-1.8034330606,	2.1186771176963387258	},
{	6.822529435e-1,	-1.7900246382,	2.1134133144277902271	},
{	6.738612652e-1,	-1.776781559,	2.1081885182208226059	},
{	6.656132936e-1,	-1.7637008429,	2.1030022361855264992	},
{	6.575059891e-1,	-1.7507799864,	2.0978541436222885817	},
{	6.495363116e-1,	-1.7380161285,	2.0927436436848862633	},
{	6.417011619e-1,	-1.7254065275,	2.0876702846488341374	},
{	6.339977384e-1,	-1.7129486799,	2.0826336266263057476	},
{	6.264231205e-1,	-1.7006399632,	2.0776332231152411511	},
{	6.189746261e-1,	-1.6884781122,	2.0726687739570012818	},
{	6.116495132e-1,	-1.6764603853,	2.0677396408370174949	},
{	6.04445219e-1,	-1.6645846367,	2.0628455642779957412	},
{	5.973591805e-1,	-1.6528484821,	2.0579861007912311779	},
{	5.903888941e-1,	-1.6412495375,	2.0531607763048084601	},
{	5.835319161e-1,	-1.6297855377,	2.0483691848079983268	},
{	5.767859221e-1,	-1.6184544563,	2.0436110475269988266	},
{	5.701485872e-1,	-1.6072540283,	2.0388858963240143691	},
{	5.636177063e-1,	-1.5961822271,	2.0341933887554151763	},
{	5.571911335e-1,	-1.5852370262,	2.0295331493314230977	},
{	5.508666039e-1,	-1.5744161606,	2.0249046951647516092	},
{	5.446421504e-1,	-1.5637179613,	2.0203078451139786342	},
{	5.385157466e-1,	-1.5531404018,	2.0157421645830040376	},
{	5.324853659e-1,	-1.5426814556,	2.0112072268967735939	},
{	5.265491009e-1,	-1.5323394537,	2.0067028352224858984	},
{	5.207051039e-1,	-1.5221124887,	2.0022285491736880257	},
{	5.149514675e-1,	-1.5119987726,	1.9977840861481438558	},
{	5.092864633e-1,	-1.5019965172,	1.9933690341102592481	},
{	5.037083626e-1,	-1.4921041727,	1.9889831990171957242	},
{	4.982153773e-1,	-1.4823198318,	1.9846261254185282453	},
{	4.928058982e-1,	-1.4726419449,	1.9802975416314198371	},
{	4.874783158e-1,	-1.4630690813,	1.9759972912404571781	},
{	4.822309911e-1,	-1.4535993338,	1.9717248212355455039	},
{	4.770624042e-1,	-1.4442312717,	1.9674799174164367906	},
{	4.719710648e-1,	-1.4349634647,	1.9632623493747335456	},
{	4.669554532e-1,	-1.425794363,	1.9590718106633419305	},
{	4.620141387e-1,	-1.4167224169,	1.9549079267278837445	},
{	4.571457505e-1,	-1.4077464342,	1.9507706079286758533	},
{	4.523488581e-1,	-1.3988647461,	1.9466593832043992863	},
{	4.476221502e-1,	-1.3900760412,	1.9425740155186810364	},
{	4.429642856e-1,	-1.3813790083,	1.9385143007378896051	},
{	4.383740127e-1,	-1.3727722168,	1.9344798518800228645	},
{	4.3385005e-1,	-1.3642544746,	1.9304705373280355767	},
{	4.293911755e-1,	-1.3558244705,	1.9264860679359023007	},
{	4.249961972e-1,	-1.3474808931,	1.9225261331954516221	},
{	4.206639528e-1,	-1.3392225504,	1.9185905143172595161	},
{	4.1639328e-1,	-1.3310482502,	1.9146789988607901377	},
{	4.121830463e-1,	-1.3229568005,	1.910791353422588999	},
{	4.080321789e-1,	-1.3149470091,	1.906927294977383146	},
{	4.039396048e-1,	-1.3070175648,	1.9030864293813255034	},
{	3.999042511e-1,	-1.2991676331,	1.8992688302508556893	},
{	3.959251344e-1,	-1.2913959026,	1.8954740297794357784	},
{	3.920012116e-1,	-1.2837014198,	1.8917019699121535739	},
{	3.881315291e-1,	-1.2760829926,	1.8879522790295079546	},
{	3.843151033e-1,	-1.2685396671,	1.8842248532007704772	},
{	3.805510104e-1,	-1.2610702515,	1.8805192998517419232	},
{	3.768383563e-1,	-1.2536740303,	1.8768356746052303484	},

/*
**	a*x^2 + b*x + c ~= sqrt(5e-1/x),		5e-1 <= x < 1.0
*/
{	1.4854669571,	-2.4854457378,	1.871356125071838183	},
{	1.4569555521,	-2.4567120075,	1.8641166687132670675	},
{	1.4292045832,	-2.428527832,	1.8569606236933505757	},
{	1.4021879435,	-2.4008784294,	1.8498863686852736748	},
{	1.3758808374,	-2.3737494946,	1.8428922507547075664	},
{	1.3502596617,	-2.3471279144,	1.8359769806847199525	},
{	1.325301528,	-2.3210003376,	1.8291390187728028981	},
{	1.3009845018,	-2.2953538895,	1.8223768737350841104	},
{	1.2772874832,	-2.2701761723,	1.815689132437547343	},
{	1.2541904449,	-2.245455265,	1.809074389036413347	},
{	1.2316738367,	-2.2211799622,	1.8025315409735020248	},
{	1.2097191811,	-2.1973388195,	1.7960591016834552054	},
{	1.1883085966,	-2.1739213467,	1.7896559762998277295	},
{	1.167424798,	-2.1509168148,	1.7833208136393936618	},
{	1.147051096,	-2.1283149719,	1.7770523917027898587	},
{	1.127171874,	-2.1061065197,	1.7708497362122495347	},
{	1.1077716351,	-2.0842814445,	1.7647114820879259773	},
{	1.088835597,	-2.0628306866,	1.7586366171474150025	},
{	1.0703496933,	-2.0417454243,	1.7526240797610809654	},
{	1.0523002148,	-2.0210170746,	1.7466728702670488513	},
{	1.0346739292,	-2.000636816,	1.7407817347114710224	},
{	1.0174583197,	-1.9805971384,	1.7349499768352952281	},
{	1.0006409883,	-1.9608895779,	1.7291763454114605346	},
{	9.84210372e-1,	-1.9415067434,	1.7234599651286684451	},
{	9.681549072e-1,	-1.9224411249,	1.7177999276044314068	},
{	9.524638057e-1,	-1.9036855698,	1.7121952995670224557	},
{	9.371263981e-1,	-1.8852329254,	1.7066451378027783911	},
{	9.221325517e-1,	-1.8670765162,	1.7011486250045946347	},
{	9.074724317e-1,	-1.8492096663,	1.6957048668596548409	},
{	8.931365609e-1,	-1.8316259384,	1.6903130118241380172	},
{	8.791157603e-1,	-1.814319253,	1.6849723465254846025	},
{	8.654011488e-1,	-1.7972832918,	1.6796819267645029511	},
{	8.519842029e-1,	-1.7805122137,	1.6744409931168894178	},
{	8.388567567e-1,	-1.764000535,	1.6692488987058947262	},
{	8.26010704e-1,	-1.7477424145,	1.6641047757586507034	},
{	8.134383559e-1,	-1.7317324877,	1.6590079164589084853	},
{	8.011323214e-1,	-1.7159655094,	1.6539575934764306191	},
{	7.890853286e-1,	-1.7004363537,	1.6489531316451486699	},
{	7.772904634e-1,	-1.6851400137,	1.6439938084540918454	},
{	7.65740931e-1,	-1.6700716019,	1.6390789513374663373	},
{	7.54430294e-1,	-1.6552265882,	1.6342079924882827583	},
{	7.433521152e-1,	-1.640599966,	1.6293800727691609523	},
{	7.325003743e-1,	-1.6261876822,	1.6245948018384631785	},
{	7.218691111e-1,	-1.6119850874,	1.6198513899786671981	},
{	7.114526629e-1,	-1.5979881287,	1.6151493315598057127	},
{	7.012453675e-1,	-1.5841923952,	1.6104879010099405966	},
{	6.912419796e-1,	-1.5705941916,	1.6058666848488655604	},
{	6.81437254e-1,	-1.5571893454,	1.6012849649345869714	},
{	6.718260646e-1,	-1.5439741611,	1.5967423114398499921	},
{	6.624036431e-1,	-1.5309448242,	1.5922380624986197111	},
{	6.531651616e-1,	-1.5180976391,	1.5877716826712331308	},
{	6.441060901e-1,	-1.5054291487,	1.5833426759719316743	},
{	6.352219582e-1,	-1.4929360151,	1.5789506181480670004	},
{	6.265084147e-1,	-1.480614543,	1.5745947905777103973	},
{	6.179613471e-1,	-1.4684617519,	1.570274875471304932	},
{	6.095765829e-1,	-1.4564743042,	1.5659903484686081951	},
{	6.013502479e-1,	-1.4446489811,	1.5617406323590747495	},
{	5.932785273e-1,	-1.4329829216,	1.5575253900948081031	},
{	5.853576064e-1,	-1.4214729071,	1.5533440417087493947	},
{	5.775840282e-1,	-1.4101163149,	1.5491962650252339879	},
{	5.699541569e-1,	-1.3989100456,	1.5450814989506893378	},
{	5.624647141e-1,	-1.3878514767,	1.5409993522652318865	},
{	5.551123023e-1,	-1.3769378662,	1.5369494240783051614	},
{	5.47893703e-1,	-1.3661663532,	1.5329311391076325034	},
{	5.408058763e-1,	-1.3555346727,	1.5289442788899148577	},
{	5.338457823e-1,	-1.3450403214,	1.5249884604662240707	},
{	5.270103812e-1,	-1.334680438,	1.521063041989517595	},
{	5.202969313e-1,	-1.3244529963,	1.5171678531392449385	},
{	5.13702631e-1,	-1.3143554926,	1.5133024075264445992	},
{	5.07224679e-1,	-1.3043856621,	1.5094664122566117415	},
{	5.008605719e-1,	-1.2945412397,	1.5056594093099230809	},
{	4.94607687e-1,	-1.2848199606,	1.5018810206540535668	},
{	4.88463521e-1,	-1.2752197981,	1.4981309930501513768	},
{	4.824256897e-1,	-1.2657386065,	1.4944089182854788839	},
{	4.764918387e-1,	-1.2563742399,	1.4907143780999090563	},
{	4.706596732e-1,	-1.2471249104,	1.4870472093725845196	},
{	4.649269581e-1,	-1.2379883528,	1.4834068433059228038	},
{	4.592915177e-1,	-1.2289628983,	1.4797931561207488266	},
{	4.537512362e-1,	-1.2200466394,	1.4762058063267732164	},
{	4.483040869e-1,	-1.2112375498,	1.4726443058277337136	},
{	4.429480433e-1,	-1.2025340796,	1.4691085600971758164	},
{	4.376811683e-1,	-1.1939343214,	1.4655981352796467727	},
{	4.325015247e-1,	-1.1854364872,	1.4621127014366889476	},
{	4.274073243e-1,	-1.1770390272,	1.4586520321860682406	},
{	4.223967195e-1,	-1.1687402725,	1.4552158519568947109	},
{	4.174679816e-1,	-1.1605386734,	1.4518039105558124454	},
{	4.126193523e-1,	-1.1524323225,	1.4484156871910555038	},
{	4.078492224e-1,	-1.1444201469,	1.4450512617287648647	},
{	4.031559229e-1,	-1.1365002394,	1.4417100643518224091	},
{	3.98537904e-1,	-1.1286712885,	1.43839194797809445	},
{	3.939936161e-1,	-1.1209317446,	1.4350965710114598912	},
{	3.895215094e-1,	-1.1132802963,	1.4318238019615985585	},
{	3.851201832e-1,	-1.1057156324,	1.4285734086210110129	},
{	3.807881474e-1,	-1.098236084,	1.4253449225241264339	},
{	3.765240312e-1,	-1.0908405781,	1.4221383066615875423	},
{	3.723264635e-1,	-1.0835276842,	1.4189532217380030405	},
{	3.681941032e-1,	-1.0762960911,	1.4157894148082008431	},
{	3.641256988e-1,	-1.0691446066,	1.4126466747119955184	},
{	3.601199389e-1,	-1.0620719194,	1.4095247373925208624	},
{	3.561756015e-1,	-1.0550769567,	1.4064234861092870078	},
{	3.522914648e-1,	-1.0481584072,	1.4033425989626974058	},
{	3.484663963e-1,	-1.0413151979,	1.4002819001954518572	},
{	3.44699204e-1,	-1.0345460176,	1.3972410535131558575	},
{	3.409888148e-1,	-1.0278499126,	1.394219952616180338	},
{	3.373340666e-1,	-1.0212256908,	1.391218355036795814	},
{	3.337339461e-1,	-1.0146723986,	1.3882361175316319908	},
{	3.301873803e-1,	-1.0081888437,	1.3852729339966112391	},
{	3.266933262e-1,	-1.0017740726,	1.3823286962165541327	},
{	3.2325086e-1,	-9.954270124e-1,	1.3794030910528882689	},
{	3.198589385e-1,	-9.891467094e-1,	1.376496020486553263	},
{	3.165166676e-1,	-9.8293221e-1,	1.3736072646635312246	},
{	3.132230639e-1,	-9.767824411e-1,	1.3707365738998594498	},
{	3.099772334e-1,	-9.706965685e-1,	1.3678838480300629548	},
{	3.067783117e-1,	-9.646735787e-1,	1.3650487974920465694	},
{	3.036254048e-1,	-9.587126374e-1,	1.3622313312325788012	},
{	3.005177081e-1,	-9.528129101e-1,	1.3594312836070771906	},
{	2.974543273e-1,	-9.469733834e-1,	1.3566484036263514053	},
{	2.944345176e-1,	-9.411932826e-1,	1.3538825358839141819	},
{	2.914574444e-1,	-9.354717731e-1,	1.3511335539557793246	},
{	2.885223329e-1,	-9.29807961e-1,	1.3484012235283039004	},
{	2.85628438e-1,	-9.242010117e-1,	1.3456853430080564922	},
{	2.827750146e-1,	-9.186502695e-1,	1.3429858882576752713	},
{	2.799613476e-1,	-9.131548405e-1,	1.3403025794889820102	},
{	2.771867216e-1,	-9.077140093e-1,	1.3376353143324014318	},
{	2.744504213e-1,	-9.023269415e-1,	1.334983877577370644	},
{	2.71751821e-1,	-8.969929814e-1,	1.3323481464845465569	},
{	2.690902054e-1,	-8.917113543e-1,	1.3297279714586007491	},
{	2.664649487e-1,	-8.864814043e-1,	1.3271232372059093815	},

};
