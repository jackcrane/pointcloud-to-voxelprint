# To process these files

You can use ADQL to query the data from the Gaia Archive at https://gea.esac.esa.int/archive/

```sql
SELECT TOP 3000000
  source_id,
  ra, dec,
  parallax, parallax_error, parallax_over_error,
  ruwe,
  phot_g_mean_mag,
  phot_bp_mean_mag,
  phot_rp_mean_mag,
  bp_rp
FROM gaiadr3.gaia_source
WHERE parallax IS NOT NULL
  AND parallax > 0
  AND parallax_over_error >= 20          -- very high SNR
  AND (1000.0 / parallax) <= 2000.0      -- d <= 2 kpc to avoid long noisy rays
  AND ruwe < 1.2                         -- well-behaved astrometric solution
  AND phot_bp_mean_mag IS NOT NULL
  AND phot_rp_mean_mag IS NOT NULL
```